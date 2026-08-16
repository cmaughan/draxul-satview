// Dual-backend C-ABI adapter for SatView: one TU compiled as C++ for Vulkan
// and as Objective-C++ for Metal (see CMakeLists), replacing the former
// satview_plugin_vk.cpp / satview_plugin_metal.mm twins. The adapter shell —
// result factories, config parse, host services, pane-state persistence,
// action registrar, and kApi assembly — comes from
// Draxul::PluginSupport::Adapter. Where the twins had drifted, the guarded
// variants won: the shared config parse never mixes iterators from two
// literals (audit bug #9) and path lookups go through HostServices' bounded
// reader.

#include <draxul/plugin_adapter.h>
#include <draxul/plugin_adapter_state.h>
#include <draxul/plugin_api.h>
#include <draxul/plugin_host_services.h>
#include <draxul/satview/satview_scene_pass.h>
#include <draxul/satview/satview_runtime.h>
#include <draxul/satview/satview_texture_assets.h>

#include "satview_imgui_adapter.h"

#if defined(__APPLE__)
#include <draxul/metal/metal_render_context.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#else
#include <draxul/vulkan/vk_plugin_allocator.h>
#include <draxul/vulkan/vk_render_context.h>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace
{

using draxul::plugin_support::kFrameDelayNs;
using draxul::plugin_support::render_result;
using draxul::plugin_support::tick_result;

constexpr const char* kSatViewPluginId = "dev.draxul.satview";

class RuntimeCallbacks final
    : public draxul::satview::SatViewRuntimeCallbacks
{
public:
    void request_frame() override
    {
        if (host && host->request_redraw)
            host->request_redraw(host->host_context);
    }
    void request_quit() override {}
    void set_window_title(std::string_view) override {}

    const DraxulPluginHostApiV2* host = nullptr;
};

struct SatViewPluginInstance
{
    explicit SatViewPluginInstance(const DraxulPluginCreateInfoV2& info)
        : host(info.host)
        , services(info)
        , directory(services.plugin_directory())
    {
    }

    const DraxulPluginHostApiV2* host = nullptr;
    draxul::plugin_support::HostServices services;
    std::filesystem::path directory;
    DraxulPluginViewportV2 viewport{};
    float speed = 1.0f;
    float angle = 0.0f;
    float direction = 1.0f;
    double last_time = -1.0;
    bool paused = false;
    bool visible = true;
    bool focused = false;
    bool quiesced = false;
    bool remember_state = false;
    std::string storage_warning;
    std::string saved_config_toml;
    std::filesystem::path data_directory;
    std::string status;
    RuntimeCallbacks runtime_callbacks;
    std::unique_ptr<draxul::plugin_support::GpuImGuiHost> imgui_overlay;
    draxul::plugin_support::UiStyleClient ui_style;
    std::unique_ptr<draxul::satview::SatViewRuntime> runtime;
#if !defined(__APPLE__)
    VmaAllocator allocator = VK_NULL_HANDLE;
#endif
};

void synchronize_ui_style(SatViewPluginInstance& instance)
{
    draxul::plugin_support::synchronize_ui_style(
        instance.ui_style, instance.runtime.get());
}

void load_saved_state(SatViewPluginInstance* instance)
{
    if (!instance->remember_state || !instance->services.has_storage())
        return;
    auto loaded = draxul::plugin_support::load_pane_state(instance->services);
    instance->storage_warning = std::move(loaded.warning);
    if (!loaded.state)
        return;
    try
    {
        const auto& state = *loaded.state;
        instance->paused = state.value("paused", instance->paused);
        const float direction = state.value("direction", instance->direction);
        instance->direction = direction < 0.0f ? -1.0f : 1.0f;
        instance->saved_config_toml = state.value(
            "satview_config_toml", std::string{});
    }
    catch (...)
    {
        instance->storage_warning = "saved state is corrupt";
    }
}

void save_state(SatViewPluginInstance* instance)
{
    if (!instance->remember_state || !instance->services.has_storage())
        return;
    nlohmann::json state{
        { "paused", instance->paused },
        { "direction", instance->direction },
    };
    if (instance->runtime)
    {
        state["satview_config_toml"]
            = draxul::satview::serialize_satview_config_toml(
                instance->runtime->current_config());
    }
    instance->storage_warning
        = draxul::plugin_support::save_pane_state(instance->services, state);
}

void* create_instance(const DraxulPluginCreateInfoV2* info)
{
    if (!info || !info->host)
        return nullptr;
    auto* instance = new SatViewPluginInstance(*info);
    draxul::satview::set_satview_asset_root(instance->directory / "assets");
    instance->viewport = info->initial_viewport;
    const auto config = draxul::plugin_support::parse_config_json(*info);
    if (!config)
    {
        delete instance;
        return nullptr;
    }
    try
    {
        instance->speed = config->value(
            "speed_radians_per_second", 1.0f);
        instance->angle = config->value("initial_angle", 0.0f);
        instance->paused = config->value("paused", false);
        instance->remember_state = config->value("remember_state", true);
    }
    catch (...)
    {
        delete instance;
        return nullptr;
    }
    instance->ui_style.discover(*instance->host);
    instance->imgui_overlay
        = draxul::plugin_support::create_gpu_imgui_host();
    if (instance->services.has_paths())
        instance->data_directory
            = instance->services.path(DRAXUL_PLUGIN_PATH_DATA);
    load_saved_state(instance);
    instance->runtime_callbacks.host = instance->host;
    instance->runtime = std::make_unique<draxul::satview::SatViewRuntime>();
    draxul::PluginRuntimeContext context;
    context.launch_options.show_ui_panels
        = instance->imgui_overlay != nullptr;
    context.initial_viewport.pixel_pos = {
        info->initial_viewport.x, info->initial_viewport.y };
    context.initial_viewport.pixel_size = {
        info->initial_viewport.width, info->initial_viewport.height };
    context.initial_viewport.pixel_scale = info->initial_viewport.pixel_scale;
    const std::filesystem::path cache_root
        = instance->services.path(DRAXUL_PLUGIN_PATH_CACHE);
    if (!instance->runtime->initialize(context,
            instance->runtime_callbacks,
            instance->directory / "assets", cache_root))
    {
        delete instance;
        return nullptr;
    }
    if (instance->paused)
        instance->runtime->dispatch_action("satview_toggle_pause");
    if (!instance->saved_config_toml.empty())
    {
        if (const auto config_toml
            = draxul::satview::parse_satview_config_toml(
                instance->saved_config_toml))
            instance->runtime->apply_config(*config_toml);
        else
            instance->storage_warning = "saved SatView preferences are corrupt";
    }
    if (instance->imgui_overlay)
        instance->runtime->attach_imgui_host(*instance->imgui_overlay);
    synchronize_ui_style(*instance);
    return instance;
}

void destroy_instance(void* opaque)
{
    auto* instance = static_cast<SatViewPluginInstance*>(opaque);
    if (!instance)
        return;
    if (instance->runtime)
        instance->runtime->shutdown();
#if !defined(__APPLE__)
    draxul::plugin_support::destroy_allocator(instance->allocator);
#endif
    delete instance;
}

void quiesce_instance(void* opaque)
{
    auto* instance = static_cast<SatViewPluginInstance*>(opaque);
    if (instance)
    {
        if (instance->runtime)
            instance->runtime->quiesce();
        save_state(instance);
        instance->quiesced = true;
    }
}

void set_viewport(void* opaque, const DraxulPluginViewportV2* viewport)
{
    if (!opaque || !viewport)
        return;
    auto* instance = static_cast<SatViewPluginInstance*>(opaque);
    instance->viewport = *viewport;
    if (instance->runtime)
    {
        draxul::PluginRuntimeViewport value;
        value.pixel_pos = { viewport->x, viewport->y };
        value.pixel_size = { viewport->width, viewport->height };
        value.pixel_scale = viewport->pixel_scale;
        instance->runtime->set_viewport(value);
    }
}

void set_visible(void* opaque, int32_t visible)
{
    auto* instance = static_cast<SatViewPluginInstance*>(opaque);
    if (!instance)
        return;
    instance->visible = visible != 0;
    instance->last_time = -1.0;
    instance->services.request_tick();
    if (instance->visible)
        instance->services.request_redraw();
    instance->services.notify_presentation_changed();
}

void set_focused(void* opaque, int32_t focused)
{
    auto* instance = static_cast<SatViewPluginInstance*>(opaque);
    if (!instance)
        return;
    instance->focused = focused != 0;
    instance->services.notify_presentation_changed();
}

void toggle_pause(SatViewPluginInstance* instance,
    bool runtime_already_toggled = false)
{
    instance->paused = !instance->paused;
    if (instance->runtime && !runtime_already_toggled)
        instance->runtime->dispatch_action("satview_toggle_pause");
    instance->last_time = -1.0;
    save_state(instance);
    instance->services.request_tick();
    instance->services.request_redraw();
    instance->services.notify_presentation_changed();
}

int32_t handle_input(void* opaque,
    const DraxulPluginInputEventV2* event)
{
    auto* instance = static_cast<SatViewPluginInstance*>(opaque);
    if (!instance || !event)
        return 0;
    if (instance->runtime)
    {
        switch (event->kind)
        {
        case DRAXUL_PLUGIN_INPUT_KEY:
            instance->runtime->on_key({
                static_cast<int>(event->physical_key), event->logical_key,
                static_cast<draxul::ModifierFlags>(event->modifiers),
                event->pressed != 0 });
            break;
        case DRAXUL_PLUGIN_INPUT_TEXT:
            instance->runtime->on_text_input({ std::string(
                event->text_utf8 ? event->text_utf8 : "",
                event->text_utf8 ? event->text_length : 0) });
            break;
        case DRAXUL_PLUGIN_INPUT_POINTER_BUTTON:
            instance->runtime->on_mouse_button({ event->button,
                event->pressed != 0,
                static_cast<draxul::ModifierFlags>(event->modifiers),
                { event->x + instance->viewport.x,
                    event->y + instance->viewport.y }, event->clicks });
            break;
        case DRAXUL_PLUGIN_INPUT_POINTER_MOVE:
            instance->runtime->on_mouse_move({
                static_cast<draxul::ModifierFlags>(event->modifiers),
                { event->x + instance->viewport.x,
                    event->y + instance->viewport.y },
                { event->delta_x, event->delta_y }, event->buttons });
            break;
        case DRAXUL_PLUGIN_INPUT_WHEEL:
            instance->runtime->on_mouse_wheel({
                { event->delta_x, event->delta_y },
                static_cast<draxul::ModifierFlags>(event->modifiers),
                { event->x + instance->viewport.x,
                    event->y + instance->viewport.y } });
            break;
        case DRAXUL_PLUGIN_INPUT_FOCUS:
            if (!event->pressed)
                instance->runtime->on_focus_lost();
            break;
        default:
            break;
        }
    }
    if (event->kind == DRAXUL_PLUGIN_INPUT_KEY
        && event->pressed && event->logical_key == 32)
    {
        toggle_pause(instance, true);
    }
    return 1;
}

DraxulPluginTickResultV2 tick(void* opaque,
    const DraxulPluginTickInfoV2* info)
{
    auto* instance = static_cast<SatViewPluginInstance*>(opaque);
    if (!instance || !info)
        return tick_result(false, DRAXUL_PLUGIN_NO_DEADLINE,
            false, "SatView received an invalid tick");
    if (instance->runtime)
    {
        synchronize_ui_style(*instance);
        instance->runtime->pump();
    }
    if (instance->quiesced || !instance->visible || !info->visible
        || instance->paused)
    {
        instance->last_time = -1.0;
        return tick_result(true, DRAXUL_PLUGIN_NO_DEADLINE);
    }
    if (instance->last_time >= 0.0)
    {
        const double elapsed = std::clamp(
            info->monotonic_seconds - instance->last_time, 0.0, 0.1);
        instance->angle += instance->speed * instance->direction
            * static_cast<float>(elapsed);
    }
    instance->last_time = info->monotonic_seconds;
    return tick_result(true, kFrameDelayNs, true);
}

#if defined(__APPLE__)

DraxulPluginRenderResultV2 render_metal(void* opaque,
    const DraxulPluginMetalFrameV2* frame)
{
    auto* instance = static_cast<SatViewPluginInstance*>(opaque);
    if (!instance || !frame || !instance->visible)
        return render_result(true, DRAXUL_PLUGIN_NO_DEADLINE);
    draxul::satview::plugin::DeferredOverlaySink sink(
        instance->imgui_overlay.get());
    if (instance->imgui_overlay)
        instance->imgui_overlay->set_metal_frame(frame);
    if (instance->runtime)
        instance->runtime->draw(sink);
    if (!sink.scene_pass())
        return render_result(false, DRAXUL_PLUGIN_NO_DEADLINE,
            "SatView runtime did not provide its scene");
    id<MTLDevice> device
        = (__bridge id<MTLDevice>)frame->device;
    id<MTLCommandBuffer> command_buffer
        = (__bridge id<MTLCommandBuffer>)frame->command_buffer;
    id<MTLTexture> texture
        = (__bridge id<MTLTexture>)frame->drawable_texture;
    MTLRenderPassDescriptor* descriptor
        = (__bridge MTLRenderPassDescriptor*)
            frame->continuation_render_pass_descriptor;
    draxul::MetalRenderContext prepass_context(command_buffer, nil,
        frame->frame_index, frame->buffered_frame_count,
        frame->framebuffer_width, frame->framebuffer_height,
        sink.scene_x(), sink.scene_y(),
        std::max(1, sink.scene_width()), std::max(1, sink.scene_height()),
        device, texture, descriptor, frame->target_generation);
    sink.scene_pass()->record_prepass(prepass_context);
    id<MTLRenderCommandEncoder> encoder
        = [command_buffer renderCommandEncoderWithDescriptor:descriptor];
    if (!encoder)
        return render_result(false, DRAXUL_PLUGIN_NO_DEADLINE,
            "SatView could not create render encoder");
    draxul::MetalRenderContext scene_context(command_buffer, encoder,
        frame->frame_index, frame->buffered_frame_count,
        frame->framebuffer_width, frame->framebuffer_height,
        sink.scene_x(), sink.scene_y(),
        std::max(1, sink.scene_width()), std::max(1, sink.scene_height()),
        device, texture, descriptor, frame->target_generation);
    sink.scene_pass()->record(scene_context);
    [encoder endEncoding];
    sink.render();
    return render_result(true, DRAXUL_PLUGIN_NO_DEADLINE);
}

#else

DraxulPluginRenderResultV2 render_vulkan(void* opaque,
    const DraxulPluginVulkanFrameV2* frame)
{
    auto* instance = static_cast<SatViewPluginInstance*>(opaque);
    if (!instance || !frame || !instance->visible)
        return render_result(true, DRAXUL_PLUGIN_NO_DEADLINE);
    draxul::satview::plugin::DeferredOverlaySink sink(
        instance->imgui_overlay.get());
    if (instance->imgui_overlay)
        instance->imgui_overlay->set_vulkan_frame(frame);
    if (instance->runtime)
        instance->runtime->draw(sink);
    static thread_local std::string error;
    error.clear();
    if (!sink.scene_pass())
        return render_result(false, DRAXUL_PLUGIN_NO_DEADLINE,
            "SatView runtime did not provide its scene");
    if (!draxul::plugin_support::ensure_allocator(instance->allocator,
            *frame, "SatView", error))
    {
        instance->services.log(DRAXUL_PLUGIN_LOG_ERROR, error);
        return render_result(false, DRAXUL_PLUGIN_NO_DEADLINE,
            error.c_str());
    }
    const auto command_buffer
        = static_cast<VkCommandBuffer>(frame->command_buffer);
    const auto render_pass = reinterpret_cast<VkRenderPass>(
        static_cast<uintptr_t>(frame->continuation_render_pass));
    draxul::VkRenderContext scene_context(command_buffer,
        static_cast<VkPhysicalDevice>(frame->physical_device),
        static_cast<VkDevice>(frame->device), instance->allocator,
        render_pass, frame->frame_index, frame->buffered_frame_count,
        frame->framebuffer_width, frame->framebuffer_height,
        sink.scene_x(), sink.scene_y(),
        std::max(1, sink.scene_width()), std::max(1, sink.scene_height()),
        reinterpret_cast<VkImage>(static_cast<uintptr_t>(frame->target_image)),
        reinterpret_cast<VkImageView>(static_cast<uintptr_t>(frame->target_image_view)),
        static_cast<VkFormat>(frame->target_format),
        static_cast<VkQueue>(frame->graphics_queue),
        frame->graphics_queue_family,
        static_cast<VkInstance>(frame->instance), render_pass,
        reinterpret_cast<VkFramebuffer>(static_cast<uintptr_t>(frame->continuation_framebuffer)),
        static_cast<VkFormat>(frame->depth_format), frame->target_generation);
    sink.scene_pass()->record_prepass(scene_context);
    VkRenderPassBeginInfo begin{
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = render_pass;
    begin.framebuffer = reinterpret_cast<VkFramebuffer>(
        static_cast<uintptr_t>(frame->continuation_framebuffer));
    begin.renderArea.extent = {
        static_cast<uint32_t>(frame->framebuffer_width),
        static_cast<uint32_t>(frame->framebuffer_height) };
    vkCmdBeginRenderPass(command_buffer, &begin,
        VK_SUBPASS_CONTENTS_INLINE);
    sink.scene_pass()->record(scene_context);
    vkCmdEndRenderPass(command_buffer);
    sink.render();
    return render_result(true, DRAXUL_PLUGIN_NO_DEADLINE);
}

#endif

int32_t get_presentation_state(void* opaque,
    DraxulPluginPresentationStateV2* state)
{
    auto* instance = static_cast<SatViewPluginInstance*>(opaque);
    if (!instance || !state
        || state->struct_size < sizeof(DraxulPluginPresentationStateV2))
        return 0;
    instance->status = instance->runtime
        ? instance->runtime->status_text() : "satview loading";
    if (!instance->visible)
        instance->status += " | hidden";
    if (instance->focused)
        instance->status += " | focused";
    if (instance->remember_state)
        instance->status += instance->services.has_storage()
            ? " | remembered" : " | storage unavailable";
    if (instance->services.has_paths() && !instance->data_directory.empty())
        instance->status += " | paths ready";
    if (!instance->storage_warning.empty())
        instance->status += " | " + instance->storage_warning;
    *state = {};
    state->struct_size = sizeof(*state);
    state->display_name = { "SatView", 7 };
    state->status_text = {
        instance->status.data(), instance->status.size() };
    state->background_red = 0.04f;
    state->background_green = 0.05f;
    state->background_blue = 0.08f;
    state->background_alpha = 1.0f;
    state->content_ready = instance->quiesced ? 0 : 1;
    state->mouse_cursor = DRAXUL_PLUGIN_CURSOR_POINTER;
    return 1;
}

int32_t dispatch_action(void* opaque, const char* action,
    size_t action_length)
{
    auto* instance = static_cast<SatViewPluginInstance*>(opaque);
    if (!instance || !action)
        return 0;
    const std::string_view value(action, action_length);
    if (value == "satview_toggle_pause")
        toggle_pause(instance);
    else if (instance->runtime
        && instance->runtime->dispatch_action(value))
    {
        save_state(instance);
        instance->services.notify_presentation_changed();
    }
    else
        return 0;
    return 1;
}

constexpr draxul::plugin_support::AdapterAction kActions[] = {
    { "toggle_ui_panels", "Toggle Control Panels" },
    { "satview_toggle_pause", "Toggle Pause" },
    { "satview_time_slower", "Slower Time" },
    { "satview_time_faster", "Faster Time" },
    { "satview_reset_camera", "Reset Camera" },
    { "satview_refresh_catalog", "Refresh Catalog" },
    { "satview_clear_selection", "Clear Selection" },
};

using Presentation = draxul::plugin_support::PresentationAdapter<kActions,
    &get_presentation_state, &dispatch_action>;

const DraxulPluginApiV2 kApi = draxul::plugin_support::make_plugin_api(
    { kSatViewPluginId, "SatView", "0.1.0",
        draxul::plugin_support::kNativeBackendMask },
    {
        .create_instance = &create_instance,
        .quiesce_instance = &quiesce_instance,
        .destroy_instance = &destroy_instance,
        .set_viewport = &set_viewport,
        .set_visible = &set_visible,
        .set_focused = &set_focused,
        .handle_input = &handle_input,
        .tick = &tick,
#if defined(__APPLE__)
        .render_metal = &render_metal,
#else
        .render_vulkan = &render_vulkan,
#endif
        .query_extension = &Presentation::query_extension,
    });

} // namespace

extern "C" DRAXUL_PLUGIN_EXPORT const DraxulPluginApiV2*
draxul_plugin_query_v2(uint32_t requested_abi)
{
    return requested_abi == DRAXUL_PLUGIN_ABI_VERSION ? &kApi : nullptr;
}
