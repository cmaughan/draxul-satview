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
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace
{

using draxul::plugin_support::kFrameDelayNs;
using draxul::plugin_support::render_result;
using draxul::plugin_support::tick_result;

constexpr const char* kSatViewPluginId = "dev.draxul.satview";

struct SatViewPluginInstance;

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
    void on_pause_changed(bool paused) override;

    const DraxulPluginHostApiV2* host = nullptr;
    SatViewPluginInstance* instance = nullptr;
};

struct SatViewPluginInstance
{
    explicit SatViewPluginInstance(const DraxulPluginCreateInfoV2& info)
        : host(info.host)
        , services(info)
        , directory(services.plugin_directory())
    {
    }

    ~SatViewPluginInstance()
    {
        // The runtime joins its catalog/cloud workers before the private
        // render-fixture cache they write into is removed.
        runtime.reset();
        if (!render_fixture_cache_directory.empty())
        {
            std::error_code ignored;
            std::filesystem::remove_all(render_fixture_cache_directory, ignored);
        }
    }

    SatViewPluginInstance(const SatViewPluginInstance&) = delete;
    SatViewPluginInstance& operator=(const SatViewPluginInstance&) = delete;

    const DraxulPluginHostApiV2* host = nullptr;
    draxul::plugin_support::HostServices services;
    std::filesystem::path directory;
    DraxulPluginViewportV2 viewport{};
    float speed = 1.0f;
    float angle = 0.0f;
    float direction = 1.0f;
    double last_time = -1.0;
    bool initial_paused = false;
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
    // Render-test-only fixture state (see parse_render_test_fixture).
    bool render_fixture = false;
    std::filesystem::path render_fixture_cache_directory;
    std::unique_ptr<draxul::satview::SatViewRuntime> runtime;
#if !defined(__APPLE__)
    VmaAllocator allocator = VK_NULL_HANDLE;
#endif
};

// --- Render-regression fixture (kanban 17) --------------------------------
// TEST-ONLY. The `render_test_fixture` launch key exists solely for the
// controlled tests/render/satview-plugin*.toml scenarios. It is not a
// SatView preference: it is never persisted, has no UI, and is ignored by
// pane-state/config.toml handling. It names offline CelesTrak GP/SATCAT
// payload files, a fixed simulation epoch, and a camera/map view; the runtime
// serves the payloads through its existing offline test transports over a
// private temporary cache, starts paused at the epoch, and reports content
// ready only once the fixture markers and tracks are on screen.
constexpr std::uintmax_t kMaxRenderFixturePayloadBytes = 4u * 1024u * 1024u;

std::filesystem::path utf8_path(const std::string& value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

bool read_render_fixture_payload(const std::filesystem::path& path,
    std::string& payload, std::string& error)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kMaxRenderFixturePayloadBytes)
    {
        error = "render_test_fixture payload is missing, empty or too large: "
            + path.generic_string();
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    payload.assign(std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
    if (!stream.good() && !stream.eof())
    {
        error = "render_test_fixture payload could not be read: "
            + path.generic_string();
        return false;
    }
    return true;
}

std::optional<double> finite_number(const nlohmann::json& object,
    const char* key, double minimum, double maximum)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number())
        return std::nullopt;
    const double value = it->get<double>();
    if (!std::isfinite(value) || value < minimum || value > maximum)
        return std::nullopt;
    return value;
}

std::optional<draxul::satview::SatViewRenderTestFixture>
parse_render_test_fixture(const nlohmann::json& value, std::string& error)
{
    draxul::satview::SatViewRenderTestFixture fixture;
    if (!value.is_object())
    {
        error = "render_test_fixture must be an object";
        return std::nullopt;
    }
    const auto unix_seconds = finite_number(value, "unix_seconds", 0.0, 4.0e9);
    const auto longitude = finite_number(value, "camera_longitude_degrees", -180.0, 180.0);
    const auto latitude = finite_number(value, "camera_latitude_degrees", -85.0, 85.0);
    const auto distance = finite_number(value, "camera_distance_earth_radii", 1.5, 20.0);
    const auto markers = finite_number(value, "required_markers", 1.0, 100000.0);
    const auto tracks = finite_number(value, "required_tracks", 0.0, 100000.0);
    const auto marker_scale = finite_number(value, "marker_scale", 1.0, 8.0);
    const auto gp_path = value.find("gp_json_path");
    const auto satcat_path = value.find("satcat_csv_path");
    const auto map_center = value.find("map_center_degrees");
    if (!unix_seconds || !longitude || !latitude || !distance || !markers
        || !tracks || !marker_scale || gp_path == value.end() || !gp_path->is_string()
        || satcat_path == value.end() || !satcat_path->is_string()
        || map_center == value.end() || !map_center->is_array()
        || map_center->size() != 2 || !(*map_center)[0].is_number()
        || !(*map_center)[1].is_number())
    {
        error = "render_test_fixture requires unix_seconds, gp_json_path, "
                "satcat_csv_path, camera_longitude_degrees, "
                "camera_latitude_degrees, camera_distance_earth_radii, "
                "map_center_degrees, marker_scale, required_markers and "
                "required_tracks";
        return std::nullopt;
    }
    const double map_longitude = (*map_center)[0].get<double>();
    const double map_latitude = (*map_center)[1].get<double>();
    if (!std::isfinite(map_longitude) || !std::isfinite(map_latitude)
        || std::abs(map_longitude) > 180.0 || std::abs(map_latitude) > 89.0)
    {
        error = "render_test_fixture map_center_degrees is out of range";
        return std::nullopt;
    }
    if (!read_render_fixture_payload(
            utf8_path(gp_path->get<std::string>()), fixture.gp_json, error)
        || !read_render_fixture_payload(
            utf8_path(satcat_path->get<std::string>()), fixture.satcat_csv, error))
        return std::nullopt;
    fixture.unix_seconds = *unix_seconds;
    fixture.camera_longitude_degrees = *longitude;
    fixture.camera_latitude_degrees = *latitude;
    fixture.camera_distance_earth_radii = static_cast<float>(*distance);
    fixture.map_center_degrees = { static_cast<float>(map_longitude),
        static_cast<float>(map_latitude) };
    fixture.marker_scale = static_cast<float>(*marker_scale);
    fixture.required_markers = static_cast<std::size_t>(*markers);
    fixture.required_tracks = static_cast<std::size_t>(*tracks);
    return fixture;
}

std::filesystem::path create_render_fixture_cache_directory(std::string& error)
{
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec)
    {
        error = "render_test_fixture has no temporary directory";
        return {};
    }
    std::random_device random;
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const std::filesystem::path candidate = base
            / ("draxul-satview-render-fixture-" + std::to_string(ticks) + "-"
                + std::to_string(random()));
        if (std::filesystem::create_directory(candidate, ec) && !ec)
            return candidate;
    }
    error = "render_test_fixture could not create a private cache directory";
    return {};
}

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
        instance->initial_paused = state.value("paused", instance->initial_paused);
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
        { "paused", instance->runtime ? instance->runtime->paused() : instance->initial_paused },
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

void RuntimeCallbacks::on_pause_changed(bool)
{
    if (!instance)
        return;
    instance->last_time = -1.0;
    save_state(instance);
    instance->services.request_tick();
    instance->services.request_redraw();
    instance->services.notify_presentation_changed();
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
    std::optional<draxul::satview::SatViewRenderTestFixture> render_fixture;
    try
    {
        instance->speed = config->value(
            "speed_radians_per_second", 1.0f);
        instance->angle = config->value("initial_angle", 0.0f);
        instance->initial_paused = config->value("paused", false);
        instance->remember_state = config->value("remember_state", true);
        instance->saved_config_toml = config->value("satview_config_toml", std::string{});
        if (const auto fixture = config->find("render_test_fixture");
            fixture != config->end())
        {
            std::string error;
            render_fixture = parse_render_test_fixture(*fixture, error);
            if (render_fixture)
                render_fixture->cache_directory
                    = create_render_fixture_cache_directory(error);
            if (!render_fixture || render_fixture->cache_directory.empty())
            {
                instance->services.log(DRAXUL_PLUGIN_LOG_ERROR,
                    "SatView: " + error);
                delete instance;
                return nullptr;
            }
            // A fixture is paused at its epoch, never remembers pane state,
            // and hides the control panels (their live timings and cache ages
            // are not part of the rendering contract).
            instance->render_fixture = true;
            instance->render_fixture_cache_directory
                = render_fixture->cache_directory;
            instance->initial_paused = true;
            instance->remember_state = false;
        }
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
        = instance->imgui_overlay != nullptr && !instance->render_fixture;
    context.initial_viewport.pixel_pos = {
        info->initial_viewport.x, info->initial_viewport.y };
    context.initial_viewport.pixel_size = {
        info->initial_viewport.width, info->initial_viewport.height };
    context.initial_viewport.pixel_scale = info->initial_viewport.pixel_scale;
    const std::filesystem::path cache_root
        = instance->services.path(DRAXUL_PLUGIN_PATH_CACHE);
    if (render_fixture
        && !instance->runtime->install_render_test_fixture(std::move(*render_fixture)))
    {
        delete instance;
        return nullptr;
    }
    if (!instance->runtime->initialize(context,
            instance->runtime_callbacks,
            instance->directory / "assets", cache_root))
    {
        delete instance;
        return nullptr;
    }
    instance->runtime->set_paused(instance->initial_paused);
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
    instance->runtime_callbacks.instance = instance;
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
    // A paused render fixture keeps pumping until its catalog, snapshot,
    // markers and tracks are on screen; afterwards it idles like any paused
    // pane. Production panes never take this branch.
    if (!instance->quiesced && instance->visible && info->visible
        && instance->runtime && instance->runtime->render_test_fixture_active()
        && !instance->runtime->render_test_fixture_ready())
        return tick_result(true, kFrameDelayNs, true);
    if (instance->quiesced || !instance->visible || !info->visible
        || (instance->runtime && instance->runtime->paused()))
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
    const bool fixture_pending = instance->runtime
        && instance->runtime->render_test_fixture_active()
        && !instance->runtime->render_test_fixture_ready();
    if (instance->render_fixture)
        instance->status += fixture_pending
            ? " | render fixture pending" : " | render fixture ready";
    *state = {};
    state->struct_size = sizeof(*state);
    state->display_name = { "SatView", 7 };
    state->status_text = {
        instance->status.data(), instance->status.size() };
    state->background_red = 0.04f;
    state->background_green = 0.05f;
    state->background_blue = 0.08f;
    state->background_alpha = 1.0f;
    state->content_ready = instance->quiesced || fixture_pending ? 0 : 1;
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
    const bool previously_paused = instance->runtime && instance->runtime->paused();
    if (instance->runtime && instance->runtime->dispatch_action(value))
    {
        if (instance->runtime->paused() == previously_paused)
        {
            save_state(instance);
            instance->services.notify_presentation_changed();
        }
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
