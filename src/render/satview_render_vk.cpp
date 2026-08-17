#include "satview_render_vk_math.h"
#include <draxul/satview/satview_scene_pass.h>
#include <draxul/satview/satview_texture_assets.h>

#include <algorithm>
#include <array>
#include <backends/imgui_impl_vulkan.h>
#include <cstddef>
#include <cstring>
#include <draxul/log.h>
#include <draxul/perf_timing.h>
#include <draxul/runtime_path.h>
#include <draxul/vulkan/vk_render_context.h>
#include <filesystem>
#include <imgui.h>
#include <limits>
#include <vector>
#include <draxul/vulkan/vk_hdr_scene_pipeline.h>
#include <draxul/vulkan/vk_resource_helpers.h>

namespace draxul::satview
{

namespace
{

constexpr size_t kEarthTextureCount = 3;
constexpr uint32_t kSatViewSamplerCount = 10;
constexpr uint32_t kBundledCloudTextureIndex = 2;
constexpr uint32_t kLiveCloudTextureBinding = 3;
constexpr uint32_t kMoonTextureBinding = 4;
constexpr uint32_t kSunTextureBinding = 5;
constexpr uint32_t kMilkyWayTextureBinding = 6;
constexpr uint32_t kLabelTextureBinding = 7;
constexpr uint32_t kFocusBodyTextureBinding = 8;
constexpr uint32_t kContextBodyTextureBinding = 9;

struct BufferResource
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
    size_t size = 0;
};

// Sampled textures, their samplers and the load-time staging upload all come
// from the shared VulkanResources helpers now.
using TextureResource = vkresources::SampledImageResource;

// Attachment creation, the per-format MSAA probe, and shader loading now live in
// Draxul::PluginSupport::VulkanResources; SatView's copies were the canonical
// ones and moved there wholesale. These aliases keep the local call sites short.
using vkresources::AttachmentResource;
using vkresources::destroy_attachment;

bool create_attachment(VkDevice device, VmaAllocator allocator, int width, int height,
    VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
    VkSampleCountFlagBits samples, VkImageCreateFlags flags, VkImageLayout final_layout,
    const char* debug_name, AttachmentResource& attachment)
{
    std::string error;
    if (vkresources::create_attachment(device, allocator,
            vkresources::AttachmentRequest(width, height, format, usage, aspect, samples,
                flags, final_layout, vkresources::LifetimeScope::Frame, debug_name),
            attachment, error))
        return true;
    DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: %s", error.c_str());
    return false;
}

uint32_t sample_count_value(VkSampleCountFlagBits samples)
{
    return static_cast<uint32_t>(samples);
}

VkShaderModule load_shader(VkDevice device, const std::string& path, bool required = true)
{
    return vkresources::load_shader(device, std::filesystem::path(path), "SatView", required);
}

void destroy_buffer(VmaAllocator allocator, BufferResource& buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
    buffer = {};
}

void destroy_texture(VkDevice device, VmaAllocator allocator, TextureResource& texture)
{
    vkresources::destroy_sampled_image(device, allocator, texture);
}

bool create_texture_resource(VkPhysicalDevice physical_device, VkDevice device, VmaAllocator allocator,
    const LoadedTextureImage& image, TextureResource& texture,
    VkFormat format = VK_FORMAT_R8G8B8A8_SRGB,
    VkSamplerAddressMode address_mode = VK_SAMPLER_ADDRESS_MODE_REPEAT)
{
    PERF_MEASURE();
    if (!image.valid())
        return false;
    std::string error;
    if (vkresources::create_sampled_image(physical_device, device, allocator,
            vkresources::SampledImageRequest(image.width, image.height, format, address_mode,
                /*generate_mips=*/false, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                vkresources::LifetimeScope::Persistent, "satview.texture"),
            texture, error))
        return true;
    DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: %s", error.c_str());
    return false;
}

bool upload_texture_immediate(const VkRenderContext& ctx, const LoadedTextureImage& image,
    TextureResource& texture, VkImageLayout old_layout)
{
    PERF_MEASURE();
    if (!image.valid() || texture.image == VK_NULL_HANDLE
        || image.width != texture.width || image.height != texture.height)
        return false;

    const vkresources::ImmediateUploadContext upload{
        ctx.physical_device(), ctx.device(), ctx.allocator(),
        ctx.graphics_queue(), ctx.graphics_queue_family()
    };
    std::string error;
    if (vkresources::upload_image_immediate(upload, image.rgba.data(), image.rgba.size(),
            texture, old_layout, error))
        return true;
    DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: %s", error.c_str());
    return false;
}

bool upload_textures_immediate(const VkRenderContext& ctx,
    const std::array<LoadedTextureImage, kEarthTextureCount>& images,
    std::array<TextureResource, kEarthTextureCount>& textures)
{
    PERF_MEASURE();
    for (size_t index = 0; index < images.size(); ++index)
    {
        if (create_texture_resource(ctx.physical_device(), ctx.device(), ctx.allocator(),
                images[index], textures[index])
            && upload_texture_immediate(ctx, images[index], textures[index],
                VK_IMAGE_LAYOUT_UNDEFINED))
            continue;
        for (auto& texture : textures)
            destroy_texture(ctx.device(), ctx.allocator(), texture);
        return false;
    }
    return true;
}

bool create_texture_immediate(
    const VkRenderContext& ctx, const LoadedTextureImage& image, TextureResource& texture,
    VkFormat format = VK_FORMAT_R8G8B8A8_SRGB,
    VkSamplerAddressMode address_mode = VK_SAMPLER_ADDRESS_MODE_REPEAT)
{
    if (!create_texture_resource(
            ctx.physical_device(), ctx.device(), ctx.allocator(), image, texture, format, address_mode))
        return false;
    if (upload_texture_immediate(ctx, image, texture, VK_IMAGE_LAYOUT_UNDEFINED))
        return true;
    destroy_texture(ctx.device(), ctx.allocator(), texture);
    return false;
}

bool update_texture_immediate(
    const VkRenderContext& ctx, const LoadedTextureImage& image, TextureResource& texture)
{
    return upload_texture_immediate(
        ctx, image, texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace

struct SatViewScenePass::State
{
    struct HdrTargets
    {
        // The MSAA colour, depth, resolve and tone-mapped attachments plus their
        // two framebuffers are the shared set; msaa_difference and its
        // framebuffer are SatView's debug view and stay here.
        vkresources::HdrSceneTargets shared;
        AttachmentResource msaa_difference;
        VkFramebuffer debug_framebuffer = VK_NULL_HANDLE;
        VkDescriptorSet post_descriptor_set = VK_NULL_HANDLE;
        VkDescriptorSet present_descriptor_set = VK_NULL_HANDLE;
        VkDescriptorSet debug_descriptor_set = VK_NULL_HANDLE;
        VkDescriptorSet imgui_hdr_descriptor_set = VK_NULL_HANDLE;
        VkDescriptorSet imgui_final_descriptor_set = VK_NULL_HANDLE;
        VkDescriptorSet imgui_msaa_descriptor_set = VK_NULL_HANDLE;
        bool debug_ready = false;

        [[nodiscard]] int width() const { return shared.width; }
        [[nodiscard]] int height() const { return shared.height; }
    };

    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkRenderPass main_render_pass = VK_NULL_HANDLE;
    // The MSAA scene pass, its MSAA-preserving twin and the tone-map pass are
    // owned by the shared HDR pipeline; these stay as plain mirrors so the draw
    // recording below is unchanged. debug_render_pass is SatView's own.
    vkresources::HdrScenePipeline hdr_pipeline;
    VkRenderPass scene_render_pass = VK_NULL_HANDLE;
    VkRenderPass scene_debug_render_pass = VK_NULL_HANDLE;
    VkRenderPass post_render_pass = VK_NULL_HANDLE;
    VkRenderPass debug_render_pass = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipelineLayout post_pipeline_layout = VK_NULL_HANDLE;
    VkPipelineLayout debug_pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout post_descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout debug_descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorPool hdr_descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkPipeline earth_pipeline = VK_NULL_HANDLE;
    VkPipeline moon_pipeline = VK_NULL_HANDLE;
    VkPipeline sun_pipeline = VK_NULL_HANDLE;
    VkPipeline body_pipeline = VK_NULL_HANDLE;
    VkPipeline context_body_pipeline = VK_NULL_HANDLE;
    VkPipeline solid_body_pipeline = VK_NULL_HANDLE;
    VkPipeline ring_pipeline = VK_NULL_HANDLE;
    VkPipeline cloud_pipeline = VK_NULL_HANDLE;
    VkPipeline atmosphere_pipeline = VK_NULL_HANDLE;
    VkPipeline ground_atmosphere_pipeline = VK_NULL_HANDLE;
    VkPipeline ground_surface_pipeline = VK_NULL_HANDLE;
    VkPipeline skybox_pipeline = VK_NULL_HANDLE;
    VkPipeline star_pipeline = VK_NULL_HANDLE;
    VkPipeline constellation_pipeline = VK_NULL_HANDLE;
    VkPipeline label_pipeline = VK_NULL_HANDLE;
    VkPipeline landscape_fill_pipeline = VK_NULL_HANDLE;
    VkPipeline landscape_rim_pipeline = VK_NULL_HANDLE;
    VkPipeline orbit_pipeline = VK_NULL_HANDLE;
    VkPipeline marker_pipeline = VK_NULL_HANDLE;
    VkPipeline tone_map_pipeline = VK_NULL_HANDLE;
    VkPipeline present_pipeline = VK_NULL_HANDLE;
    VkPipeline msaa_debug_pipeline = VK_NULL_HANDLE;
    VkSampler hdr_sampler = VK_NULL_HANDLE;
    VkSampleCountFlagBits scene_sample_count = VK_SAMPLE_COUNT_1_BIT;
    std::vector<HdrTargets> hdr_targets;
    uint32_t last_prepass_frame = 0;
    std::array<TextureResource, kEarthTextureCount> earth_textures{};
    TextureResource moon_texture;
    TextureResource sun_texture;
    TextureResource milky_way_texture;
    TextureResource live_cloud_texture;
    TextureResource label_texture;
    TextureResource focus_body_texture;
    TextureResource context_body_texture;
    BufferResource track_vertex_buffer;
    BufferResource earth_track_vertex_buffer;
    BufferResource marker_buffer;
    BufferResource surface_marker_buffer;
    BufferResource star_buffer;
    BufferResource constellation_buffer;
    BufferResource constellation_boundary_buffer;
    BufferResource constellation_label_buffer;
    BufferResource cardinal_label_buffer;
    BufferResource observatory_fill_buffer;
    BufferResource observatory_rim_buffer;
    uint32_t track_vertex_count = 0;
    uint32_t earth_track_vertex_count = 0;
    uint32_t marker_count = 0;
    uint32_t surface_marker_count = 0;
    uint32_t star_count = 0;
    uint32_t constellation_line_count = 0;
    uint32_t constellation_boundary_line_count = 0;
    uint32_t constellation_label_count = 0;
    uint32_t cardinal_label_count = 0;
    uint32_t observatory_fill_triangle_count = 0;
    uint32_t observatory_rim_line_count = 0;
    uint64_t uploaded_track_revision = 0;
    uint64_t uploaded_earth_track_revision = 0;
    uint64_t uploaded_marker_revision = 0;
    uint64_t uploaded_surface_marker_revision = 0;
    uint64_t uploaded_star_revision = 0;
    uint64_t uploaded_constellation_revision = 0;
    uint64_t uploaded_constellation_boundary_revision = 0;
    uint64_t uploaded_constellation_label_revision = 0;
    uint64_t uploaded_cardinal_label_revision = 0;
    uint64_t uploaded_observatory_revision = 0;
    uint64_t uploaded_observatory_rim_revision = 0;
    uint64_t uploaded_label_atlas_revision = 0;
    uint64_t uploaded_cloud_revision = 0;
    int uploaded_focus_body = -1;
    int uploaded_context_body = -1;

    ~State()
    {
        destroy();
    }

    void destroy_pipelines()
    {
        if (device != VK_NULL_HANDLE)
        {
            if (earth_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, earth_pipeline, nullptr);
            if (moon_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, moon_pipeline, nullptr);
            if (sun_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, sun_pipeline, nullptr);
            if (body_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, body_pipeline, nullptr);
            if (context_body_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, context_body_pipeline, nullptr);
            if (solid_body_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, solid_body_pipeline, nullptr);
            if (ring_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, ring_pipeline, nullptr);
            if (cloud_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, cloud_pipeline, nullptr);
            if (atmosphere_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, atmosphere_pipeline, nullptr);
            if (ground_atmosphere_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, ground_atmosphere_pipeline, nullptr);
            if (ground_surface_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, ground_surface_pipeline, nullptr);
            if (skybox_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, skybox_pipeline, nullptr);
            if (star_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, star_pipeline, nullptr);
            if (constellation_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, constellation_pipeline, nullptr);
            if (label_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, label_pipeline, nullptr);
            if (landscape_fill_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, landscape_fill_pipeline, nullptr);
            if (landscape_rim_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, landscape_rim_pipeline, nullptr);
            if (orbit_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, orbit_pipeline, nullptr);
            if (marker_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, marker_pipeline, nullptr);
        }
        earth_pipeline = VK_NULL_HANDLE;
        moon_pipeline = VK_NULL_HANDLE;
        sun_pipeline = VK_NULL_HANDLE;
        body_pipeline = VK_NULL_HANDLE;
        context_body_pipeline = VK_NULL_HANDLE;
        solid_body_pipeline = VK_NULL_HANDLE;
        ring_pipeline = VK_NULL_HANDLE;
        cloud_pipeline = VK_NULL_HANDLE;
        atmosphere_pipeline = VK_NULL_HANDLE;
        ground_atmosphere_pipeline = VK_NULL_HANDLE;
        ground_surface_pipeline = VK_NULL_HANDLE;
        skybox_pipeline = VK_NULL_HANDLE;
        star_pipeline = VK_NULL_HANDLE;
        constellation_pipeline = VK_NULL_HANDLE;
        label_pipeline = VK_NULL_HANDLE;
        landscape_fill_pipeline = VK_NULL_HANDLE;
        landscape_rim_pipeline = VK_NULL_HANDLE;
        orbit_pipeline = VK_NULL_HANDLE;
        marker_pipeline = VK_NULL_HANDLE;
    }

    void destroy_hdr_targets()
    {
        if (device == VK_NULL_HANDLE || allocator == VK_NULL_HANDLE)
        {
            hdr_targets.clear();
            return;
        }
        for (auto& targets : hdr_targets)
        {
            if (ImGui::GetCurrentContext())
            {
                if (targets.imgui_hdr_descriptor_set != VK_NULL_HANDLE)
                    ImGui_ImplVulkan_RemoveTexture(targets.imgui_hdr_descriptor_set);
                if (targets.imgui_final_descriptor_set != VK_NULL_HANDLE)
                    ImGui_ImplVulkan_RemoveTexture(targets.imgui_final_descriptor_set);
                if (targets.imgui_msaa_descriptor_set != VK_NULL_HANDLE)
                    ImGui_ImplVulkan_RemoveTexture(targets.imgui_msaa_descriptor_set);
            }
            if (targets.debug_framebuffer != VK_NULL_HANDLE)
                vkDestroyFramebuffer(device, targets.debug_framebuffer, nullptr);
            targets.debug_framebuffer = VK_NULL_HANDLE;
            destroy_attachment(device, allocator, targets.msaa_difference);
            vkresources::destroy_hdr_scene_targets(device, allocator, targets.shared);
        }
        hdr_targets.clear();
        if (hdr_descriptor_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device, hdr_descriptor_pool, nullptr);
        hdr_descriptor_pool = VK_NULL_HANDLE;
    }

    void destroy_hdr_resources()
    {
        destroy_hdr_targets();
        if (device != VK_NULL_HANDLE)
        {
            if (tone_map_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, tone_map_pipeline, nullptr);
            if (present_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, present_pipeline, nullptr);
            if (msaa_debug_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, msaa_debug_pipeline, nullptr);
            if (post_pipeline_layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, post_pipeline_layout, nullptr);
            if (debug_pipeline_layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, debug_pipeline_layout, nullptr);
            if (post_descriptor_set_layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, post_descriptor_set_layout, nullptr);
            if (debug_descriptor_set_layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, debug_descriptor_set_layout, nullptr);
            if (hdr_sampler != VK_NULL_HANDLE)
                vkDestroySampler(device, hdr_sampler, nullptr);
            hdr_pipeline.destroy(device);
            if (debug_render_pass != VK_NULL_HANDLE)
                vkDestroyRenderPass(device, debug_render_pass, nullptr);
        }
        tone_map_pipeline = VK_NULL_HANDLE;
        present_pipeline = VK_NULL_HANDLE;
        msaa_debug_pipeline = VK_NULL_HANDLE;
        post_pipeline_layout = VK_NULL_HANDLE;
        debug_pipeline_layout = VK_NULL_HANDLE;
        post_descriptor_set_layout = VK_NULL_HANDLE;
        debug_descriptor_set_layout = VK_NULL_HANDLE;
        hdr_sampler = VK_NULL_HANDLE;
        scene_render_pass = VK_NULL_HANDLE;
        scene_debug_render_pass = VK_NULL_HANDLE;
        post_render_pass = VK_NULL_HANDLE;
        debug_render_pass = VK_NULL_HANDLE;
        main_render_pass = VK_NULL_HANDLE;
        scene_sample_count = VK_SAMPLE_COUNT_1_BIT;
    }

    void destroy()
    {
        if (device != VK_NULL_HANDLE)
        {
            destroy_pipelines();
            destroy_hdr_resources();
            if (layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, layout, nullptr);
            if (descriptor_pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            if (descriptor_set_layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
            if (allocator != VK_NULL_HANDLE)
            {
                for (auto& texture : earth_textures)
                    destroy_texture(device, allocator, texture);
                destroy_texture(device, allocator, moon_texture);
                destroy_texture(device, allocator, sun_texture);
                destroy_texture(device, allocator, milky_way_texture);
                destroy_texture(device, allocator, live_cloud_texture);
                destroy_texture(device, allocator, label_texture);
                destroy_texture(device, allocator, focus_body_texture);
                destroy_texture(device, allocator, context_body_texture);
                destroy_buffer(allocator, track_vertex_buffer);
                destroy_buffer(allocator, earth_track_vertex_buffer);
                destroy_buffer(allocator, marker_buffer);
                destroy_buffer(allocator, surface_marker_buffer);
                destroy_buffer(allocator, star_buffer);
                destroy_buffer(allocator, constellation_buffer);
                destroy_buffer(allocator, constellation_boundary_buffer);
                destroy_buffer(allocator, constellation_label_buffer);
                destroy_buffer(allocator, cardinal_label_buffer);
                destroy_buffer(allocator, observatory_fill_buffer);
                destroy_buffer(allocator, observatory_rim_buffer);
            }
        }
        layout = VK_NULL_HANDLE;
        descriptor_set_layout = VK_NULL_HANDLE;
        descriptor_pool = VK_NULL_HANDLE;
        descriptor_set = VK_NULL_HANDLE;
        render_pass = VK_NULL_HANDLE;
        main_render_pass = VK_NULL_HANDLE;
        allocator = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        track_vertex_count = 0;
        earth_track_vertex_count = 0;
        marker_count = 0;
        surface_marker_count = 0;
        star_count = 0;
        constellation_line_count = 0;
        constellation_boundary_line_count = 0;
        constellation_label_count = 0;
        cardinal_label_count = 0;
        observatory_fill_triangle_count = 0;
        observatory_rim_line_count = 0;
        uploaded_track_revision = 0;
        uploaded_earth_track_revision = 0;
        uploaded_marker_revision = 0;
        uploaded_surface_marker_revision = 0;
        uploaded_star_revision = 0;
        uploaded_constellation_revision = 0;
        uploaded_constellation_boundary_revision = 0;
        uploaded_constellation_label_revision = 0;
        uploaded_cardinal_label_revision = 0;
        uploaded_observatory_revision = 0;
        uploaded_observatory_rim_revision = 0;
        uploaded_label_atlas_revision = 0;
        uploaded_cloud_revision = 0;
        uploaded_focus_body = -1;
        uploaded_context_body = -1;
    }

    bool ensure_texture_descriptors(const VkRenderContext& ctx)
    {
        if (descriptor_set != VK_NULL_HANDLE && device == ctx.device() && allocator == ctx.allocator())
            return true;

        if (device != VK_NULL_HANDLE && (device != ctx.device() || allocator != ctx.allocator()))
            destroy();

        device = ctx.device();
        allocator = ctx.allocator();
        if (device == VK_NULL_HANDLE || allocator == VK_NULL_HANDLE)
            return false;

        EarthTextureImages images = load_earth_texture_images();
        std::array<LoadedTextureImage, kEarthTextureCount> image_array = {
            std::move(images.day),
            std::move(images.night),
            std::move(images.clouds),
        };
        if (!upload_textures_immediate(ctx, image_array, earth_textures))
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to upload Earth textures");
            destroy();
            return false;
        }
        const LoadedTextureImage moon_image = load_moon_texture_image();
        if (!create_texture_immediate(ctx, moon_image, moon_texture))
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to upload Moon texture");
            destroy();
            return false;
        }
        const LoadedTextureImage sun_image = load_sun_texture_image();
        if (!create_texture_immediate(ctx, sun_image, sun_texture))
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to upload Sun texture");
            destroy();
            return false;
        }
        const LoadedTextureImage milky_way_image = load_milky_way_texture_image();
        if (!create_texture_immediate(ctx, milky_way_image, milky_way_texture))
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to upload Milky Way texture");
            destroy();
            return false;
        }
        const LoadedTextureImage transparent_label{ 1, 1, { 0, 0, 0, 0 } };
        if (!create_texture_immediate(
                ctx,
                transparent_label,
                label_texture,
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE))
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to create label atlas texture");
            destroy();
            return false;
        }
        const LoadedTextureImage fallback_body{ 1, 1, { 128, 128, 128, 255 } };
        if (!create_texture_immediate(ctx, fallback_body, focus_body_texture))
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to create focus body texture");
            destroy();
            return false;
        }
        if (!create_texture_immediate(ctx, fallback_body, context_body_texture))
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to create context body texture");
            destroy();
            return false;
        }

        VkDescriptorSetLayoutBinding bindings[kSatViewSamplerCount]{};
        for (uint32_t index = 0; index < kSatViewSamplerCount; ++index)
        {
            bindings[index].binding = index;
            bindings[index].descriptorCount = 1;
            bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo layout_ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layout_ci.bindingCount = static_cast<uint32_t>(std::size(bindings));
        layout_ci.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &layout_ci, nullptr, &descriptor_set_layout) != VK_SUCCESS)
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to create texture descriptor layout");
            destroy();
            return false;
        }

        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(SatViewFrameUniforms);

        VkPipelineLayoutCreateInfo pipeline_layout_ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipeline_layout_ci.setLayoutCount = 1;
        pipeline_layout_ci.pSetLayouts = &descriptor_set_layout;
        pipeline_layout_ci.pushConstantRangeCount = 1;
        pipeline_layout_ci.pPushConstantRanges = &push_range;
        if (vkCreatePipelineLayout(device, &pipeline_layout_ci, nullptr, &layout) != VK_SUCCESS)
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to create pipeline layout");
            destroy();
            return false;
        }

        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = kSatViewSamplerCount;

        VkDescriptorPoolCreateInfo pool_ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_ci.maxSets = 1;
        pool_ci.poolSizeCount = 1;
        pool_ci.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(device, &pool_ci, nullptr, &descriptor_pool) != VK_SUCCESS)
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to create texture descriptor pool");
            destroy();
            return false;
        }

        VkDescriptorSetAllocateInfo alloc_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc_info.descriptorPool = descriptor_pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &descriptor_set_layout;
        if (vkAllocateDescriptorSets(device, &alloc_info, &descriptor_set) != VK_SUCCESS)
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to allocate texture descriptor set");
            destroy();
            return false;
        }

        std::array<VkDescriptorImageInfo, kSatViewSamplerCount> image_infos{};
        std::array<VkWriteDescriptorSet, kSatViewSamplerCount> writes{};
        for (uint32_t index = 0; index < kSatViewSamplerCount; ++index)
        {
            const TextureResource* texture = nullptr;
            if (index == kMilkyWayTextureBinding)
                texture = &milky_way_texture;
            else if (index == kFocusBodyTextureBinding)
                texture = &focus_body_texture;
            else if (index == kContextBodyTextureBinding)
                texture = &context_body_texture;
            else if (index == kLabelTextureBinding)
                texture = &label_texture;
            else if (index == kSunTextureBinding)
                texture = &sun_texture;
            else if (index == kMoonTextureBinding)
                texture = &moon_texture;
            else if (index == kLiveCloudTextureBinding)
                texture = &earth_textures[kBundledCloudTextureIndex];
            else
                texture = &earth_textures[index];
            image_infos[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            image_infos[index].imageView = texture->view;
            image_infos[index].sampler = texture->sampler;

            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = descriptor_set;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[index].pImageInfo = &image_infos[index];
        }
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        return true;
    }

    bool ensure_cloud_texture(
        const VkRenderContext& ctx, const LoadedTextureImage& image, uint64_t revision)
    {
        if (revision == uploaded_cloud_revision)
            return true;

        bool uploaded = false;
        if (live_cloud_texture.image == VK_NULL_HANDLE
            || image.width != live_cloud_texture.width
            || image.height != live_cloud_texture.height)
        {
            destroy_texture(ctx.device(), ctx.allocator(), live_cloud_texture);
            uploaded = create_texture_immediate(ctx, image, live_cloud_texture);
        }
        else
        {
            uploaded = update_texture_immediate(ctx, image, live_cloud_texture);
        }
        if (!uploaded)
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to upload live cloud texture");
            return false;
        }

        VkDescriptorImageInfo image_info{};
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_info.imageView = live_cloud_texture.view;
        image_info.sampler = live_cloud_texture.sampler;
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = descriptor_set;
        write.dstBinding = kLiveCloudTextureBinding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        uploaded_cloud_revision = revision;
        return true;
    }

    bool ensure_focus_body_texture(const VkRenderContext& ctx, SatViewCameraPov body)
    {
        const int body_value = static_cast<int>(body);
        if (uploaded_focus_body == body_value)
            return true;

        const LoadedTextureImage image = load_solar_system_body_texture_image(body);
        if (!image.valid())
            return false;
        // POV changes are user-driven and rare. Waiting here keeps the previous
        // texture alive until all in-flight scene command buffers have finished.
        if (focus_body_texture.image != VK_NULL_HANDLE)
            vkDeviceWaitIdle(ctx.device());
        destroy_texture(ctx.device(), ctx.allocator(), focus_body_texture);
        if (!create_texture_immediate(ctx, image, focus_body_texture))
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to upload focus body texture");
            return false;
        }

        VkDescriptorImageInfo image_info{};
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_info.imageView = focus_body_texture.view;
        image_info.sampler = focus_body_texture.sampler;
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = descriptor_set;
        write.dstBinding = kFocusBodyTextureBinding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        uploaded_focus_body = body_value;
        return true;
    }

    bool ensure_context_body_texture(const VkRenderContext& ctx, SatViewCameraPov body)
    {
        const int body_value = static_cast<int>(body);
        if (uploaded_context_body == body_value)
            return true;

        const LoadedTextureImage image = load_solar_system_body_texture_image(body);
        if (!image.valid())
            return false;
        if (context_body_texture.image != VK_NULL_HANDLE)
            vkDeviceWaitIdle(ctx.device());
        destroy_texture(ctx.device(), ctx.allocator(), context_body_texture);
        if (!create_texture_immediate(ctx, image, context_body_texture))
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to upload context body texture");
            return false;
        }

        VkDescriptorImageInfo image_info{};
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_info.imageView = context_body_texture.view;
        image_info.sampler = context_body_texture.sampler;
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = descriptor_set;
        write.dstBinding = kContextBodyTextureBinding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        uploaded_context_body = body_value;
        return true;
    }

    template <typename T>
    bool ensure_vertex_buffer(const VkRenderContext& ctx,
        const std::vector<T>& items,
        uint64_t revision,
        BufferResource& buffer,
        uint32_t& item_count,
        uint64_t& uploaded_revision)
    {
        if (revision == uploaded_revision)
            return true;

        item_count = static_cast<uint32_t>(
            std::min<std::size_t>(items.size(), std::numeric_limits<uint32_t>::max()));
        if (item_count == 0)
        {
            uploaded_revision = revision;
            return true;
        }

        const size_t byte_size = static_cast<size_t>(item_count) * sizeof(T);
        if (buffer.buffer == VK_NULL_HANDLE || buffer.size < byte_size)
        {
            if (buffer.buffer != VK_NULL_HANDLE)
                destroy_buffer(ctx.allocator(), buffer);

            VkBufferCreateInfo buf_ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            buf_ci.size = byte_size;
            buf_ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            buf_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo alloc_ci{};
            alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                | VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo alloc_info{};
            if (vmaCreateBuffer(ctx.allocator(), &buf_ci, &alloc_ci,
                    &buffer.buffer, &buffer.allocation, &alloc_info)
                != VK_SUCCESS)
            {
                buffer = {};
                item_count = 0;
                return false;
            }

            buffer.mapped = alloc_info.pMappedData;
            buffer.size = byte_size;
        }

        if (!buffer.mapped)
        {
            item_count = 0;
            return false;
        }

        std::memcpy(buffer.mapped, items.data(), byte_size);
        vmaFlushAllocation(ctx.allocator(), buffer.allocation, 0, byte_size);
        uploaded_revision = revision;
        return true;
    }

    bool ensure_label_texture(
        const VkRenderContext& ctx,
        const TextAtlasImage& image,
        uint64_t revision)
    {
        if (revision == uploaded_label_atlas_revision)
            return true;
        LoadedTextureImage upload_image{ image.width, image.height, image.rgba };
        bool uploaded = false;
        if (label_texture.image == VK_NULL_HANDLE
            || image.width != label_texture.width
            || image.height != label_texture.height)
        {
            destroy_texture(ctx.device(), ctx.allocator(), label_texture);
            uploaded = create_texture_immediate(
                ctx,
                upload_image,
                label_texture,
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        }
        else
        {
            uploaded = update_texture_immediate(ctx, upload_image, label_texture);
        }
        if (!uploaded)
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to upload label atlas texture");
            return false;
        }

        VkDescriptorImageInfo image_info{};
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_info.imageView = label_texture.view;
        image_info.sampler = label_texture.sampler;
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = descriptor_set;
        write.dstBinding = kLabelTextureBinding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        uploaded_label_atlas_revision = revision;
        return true;
    }

    bool create_hdr_render_passes(VkPhysicalDevice physical_device)
    {
        // The MSAA scene pass, its MSAA-preserving twin and the tone-map pass
        // come from the shared HDR pipeline, which owns the one set of subpass
        // dependency masks both products now use. SatView's MSAA-difference
        // debug pass is its own, but uses the shared colour-pass masks.
        std::string error;
        vkresources::HdrScenePipelineConfig config;
        config.color_format = VK_FORMAT_R16G16B16A16_SFLOAT;
        config.depth_format = VK_FORMAT_D32_SFLOAT;
        config.tone_mapped_format = VK_FORMAT_B8G8R8A8_SRGB;
        config.tone_map_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
        config.want_msaa_preserving_pass = true;
        config.debug_name = "SatView";
        if (!hdr_pipeline.create(physical_device, device, config, error))
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: %s", error.c_str());
            return false;
        }
        scene_sample_count = hdr_pipeline.sample_count();
        scene_render_pass = hdr_pipeline.scene_render_pass();
        scene_debug_render_pass = hdr_pipeline.scene_msaa_preserving_render_pass();
        post_render_pass = hdr_pipeline.tone_map_render_pass();

        if (!vkresources::create_color_render_pass(device, VK_FORMAT_R8G8B8A8_UNORM,
                VK_ATTACHMENT_LOAD_OP_CLEAR, debug_render_pass, error))
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: %s", error.c_str());
            return false;
        }
        return true;
    }

    bool create_hdr_layouts()
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorCount = 1;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo set_ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        set_ci.bindingCount = 1;
        set_ci.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(device, &set_ci, nullptr, &post_descriptor_set_layout) != VK_SUCCESS
            || vkCreateDescriptorSetLayout(device, &set_ci, nullptr, &debug_descriptor_set_layout) != VK_SUCCESS)
            return false;

        VkPushConstantRange post_push{};
        post_push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        post_push.size = sizeof(glm::vec4);
        VkPipelineLayoutCreateInfo post_ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        post_ci.setLayoutCount = 1;
        post_ci.pSetLayouts = &post_descriptor_set_layout;
        post_ci.pushConstantRangeCount = 1;
        post_ci.pPushConstantRanges = &post_push;
        if (vkCreatePipelineLayout(device, &post_ci, nullptr, &post_pipeline_layout) != VK_SUCCESS)
            return false;

        VkPushConstantRange debug_push{};
        debug_push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        debug_push.size = sizeof(glm::ivec4);
        VkPipelineLayoutCreateInfo debug_ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        debug_ci.setLayoutCount = 1;
        debug_ci.pSetLayouts = &debug_descriptor_set_layout;
        debug_ci.pushConstantRangeCount = 1;
        debug_ci.pPushConstantRanges = &debug_push;
        if (vkCreatePipelineLayout(device, &debug_ci, nullptr, &debug_pipeline_layout) != VK_SUCCESS)
            return false;

        VkSamplerCreateInfo sampler_ci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler_ci.magFilter = VK_FILTER_LINEAR;
        sampler_ci.minFilter = VK_FILTER_LINEAR;
        sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        return vkCreateSampler(device, &sampler_ci, nullptr, &hdr_sampler) == VK_SUCCESS;
    }

    bool create_hdr_pipelines()
    {
        // Tone-map, present and MSAA-debug are all the same fullscreen-triangle
        // state vector, which the shared helper now owns.
        const auto shader_dir = resolve_satview_asset_path("../shaders");
        VkShaderModule vert = load_shader(device, (shader_dir / "satview_post.vert.spv").string());
        VkShaderModule post_frag = load_shader(device, (shader_dir / "satview_post.frag.spv").string());
        VkShaderModule present_frag = load_shader(device, (shader_dir / "satview_present.frag.spv").string());
        VkShaderModule debug_frag = scene_sample_count == VK_SAMPLE_COUNT_1_BIT
            ? VK_NULL_HANDLE
            : load_shader(device, (shader_dir / "satview_msaa_debug.frag.spv").string());
        const auto destroy_modules = [&]() {
            if (vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, vert, nullptr);
            if (post_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, post_frag, nullptr);
            if (present_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, present_frag, nullptr);
            if (debug_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, debug_frag, nullptr);
        };
        if (vert == VK_NULL_HANDLE || post_frag == VK_NULL_HANDLE || present_frag == VK_NULL_HANDLE
            || (scene_sample_count != VK_SAMPLE_COUNT_1_BIT && debug_frag == VK_NULL_HANDLE))
        {
            destroy_modules();
            return false;
        }

        std::string error;
        const auto create = [&](VkShaderModule fragment, VkPipelineLayout pipeline_layout,
                                VkRenderPass pipeline_render_pass, VkPipeline& pipeline) {
            if (vkresources::create_fullscreen_pipeline(device,
                    vkresources::FullscreenPipelineRequest(vert, fragment, pipeline_layout,
                        pipeline_render_pass),
                    pipeline, error))
                return true;
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: %s", error.c_str());
            return false;
        };

        const bool result = create(post_frag, post_pipeline_layout, post_render_pass, tone_map_pipeline)
            && create(present_frag, post_pipeline_layout, main_render_pass, present_pipeline)
            && (scene_sample_count == VK_SAMPLE_COUNT_1_BIT
                || create(debug_frag, debug_pipeline_layout, debug_render_pass, msaa_debug_pipeline));
        destroy_modules();
        return result;
    }

    bool ensure_hdr_setup(const VkRenderContext& ctx)
    {
        // The shared probe checks per-format sample support (audit bug #7), so
        // the answer can differ from the old limits-only query on some devices.
        const VkSampleCountFlagBits selected_samples = vkresources::choose_scene_sample_count(
            ctx.physical_device(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT);
        if (scene_render_pass != VK_NULL_HANDLE
            && tone_map_pipeline != VK_NULL_HANDLE
            && present_pipeline != VK_NULL_HANDLE
            && main_render_pass == ctx.render_pass()
            && scene_sample_count == selected_samples)
            return true;

        if (device == VK_NULL_HANDLE || allocator == VK_NULL_HANDLE)
            return false;
        vkDeviceWaitIdle(device);
        destroy_pipelines();
        destroy_hdr_resources();
        main_render_pass = ctx.render_pass();
        if (!create_hdr_render_passes(ctx.physical_device()) || !create_hdr_layouts())
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to create HDR render-pass resources");
            return false;
        }
        if (!ensure_pipelines(device, scene_render_pass) || !create_hdr_pipelines())
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to create HDR pipelines");
            return false;
        }
        return true;
    }

    bool ensure_hdr_targets(uint32_t frame_count, int width, int height)
    {
        frame_count = std::max(1u, frame_count);
        if (hdr_targets.size() == frame_count && !hdr_targets.empty()
            && hdr_targets.front().width() == width && hdr_targets.front().height() == height)
            return true;

        vkDeviceWaitIdle(device);
        destroy_hdr_targets();
        hdr_targets.resize(frame_count);

        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = frame_count * 3u;
        VkDescriptorPoolCreateInfo pool_ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_ci.maxSets = frame_count * 3u;
        pool_ci.poolSizeCount = 1;
        pool_ci.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(device, &pool_ci, nullptr, &hdr_descriptor_pool) != VK_SUCCESS)
            return false;

        const bool multisampled = hdr_pipeline.multisampled();
        for (auto& targets : hdr_targets)
        {
            // The MSAA colour / depth / resolve / tone-mapped attachments and
            // their two framebuffers are the shared set. Only SatView's
            // MSAA-difference debug target is built here.
            std::string error;
            if (!hdr_pipeline.create_targets(device, allocator, width, height, targets.shared, error))
            {
                DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: %s", error.c_str());
                return false;
            }
            if (!create_attachment(device, allocator, width, height,
                    VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    "satview.hdr.msaa-difference", targets.msaa_difference))
                return false;

            VkFramebufferCreateInfo debug_fb_ci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            debug_fb_ci.renderPass = debug_render_pass;
            debug_fb_ci.attachmentCount = 1;
            debug_fb_ci.pAttachments = &targets.msaa_difference.view;
            debug_fb_ci.width = static_cast<uint32_t>(width);
            debug_fb_ci.height = static_cast<uint32_t>(height);
            debug_fb_ci.layers = 1;
            if (vkCreateFramebuffer(device, &debug_fb_ci, nullptr, &targets.debug_framebuffer) != VK_SUCCESS)
                return false;

            std::array<VkDescriptorSetLayout, 3> layouts = {
                post_descriptor_set_layout,
                post_descriptor_set_layout,
                debug_descriptor_set_layout,
            };
            std::array<VkDescriptorSet, 3> sets{};
            VkDescriptorSetAllocateInfo alloc_ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            alloc_ci.descriptorPool = hdr_descriptor_pool;
            alloc_ci.descriptorSetCount = multisampled ? 3u : 2u;
            alloc_ci.pSetLayouts = layouts.data();
            if (vkAllocateDescriptorSets(device, &alloc_ci, sets.data()) != VK_SUCCESS)
                return false;
            targets.post_descriptor_set = sets[0];
            targets.present_descriptor_set = sets[1];
            targets.debug_descriptor_set = multisampled ? sets[2] : VK_NULL_HANDLE;

            std::array<VkDescriptorImageInfo, 3> image_infos{};
            image_infos[0] = { hdr_sampler, targets.shared.scene_hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            image_infos[1] = { hdr_sampler, targets.shared.scene_final_unorm_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            image_infos[2] = { hdr_sampler, targets.shared.scene_msaa.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            std::array<VkWriteDescriptorSet, 3> writes{};
            const uint32_t write_count = multisampled ? 3u : 2u;
            for (uint32_t i = 0; i < write_count; ++i)
            {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = sets[i];
                writes[i].dstBinding = 0;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &image_infos[i];
            }
            vkUpdateDescriptorSets(device, write_count, writes.data(), 0, nullptr);
        }
        return true;
    }

    bool ensure_pipelines(VkDevice new_device, VkRenderPass new_render_pass)
    {
        if (earth_pipeline != VK_NULL_HANDLE && moon_pipeline != VK_NULL_HANDLE
            && sun_pipeline != VK_NULL_HANDLE
            && body_pipeline != VK_NULL_HANDLE
            && context_body_pipeline != VK_NULL_HANDLE
            && solid_body_pipeline != VK_NULL_HANDLE
            && ring_pipeline != VK_NULL_HANDLE
            && cloud_pipeline != VK_NULL_HANDLE
            && atmosphere_pipeline != VK_NULL_HANDLE
            && ground_atmosphere_pipeline != VK_NULL_HANDLE
            && ground_surface_pipeline != VK_NULL_HANDLE
            && skybox_pipeline != VK_NULL_HANDLE
            && star_pipeline != VK_NULL_HANDLE
            && constellation_pipeline != VK_NULL_HANDLE
            && label_pipeline != VK_NULL_HANDLE
            && landscape_fill_pipeline != VK_NULL_HANDLE
            && landscape_rim_pipeline != VK_NULL_HANDLE
            && orbit_pipeline != VK_NULL_HANDLE
            && device == new_device && render_pass == new_render_pass)
            return true;

        destroy_pipelines();
        device = new_device;
        render_pass = new_render_pass;
        if (device == VK_NULL_HANDLE || render_pass == VK_NULL_HANDLE || layout == VK_NULL_HANDLE)
            return false;

        const auto shader_dir = resolve_satview_asset_path("../shaders");
        VkShaderModule vert = load_shader(device, (shader_dir / "satview_earth.vert.spv").string());
        VkShaderModule frag = load_shader(device, (shader_dir / "satview_earth.frag.spv").string());
        VkShaderModule moon_vert = load_shader(device, (shader_dir / "satview_moon.vert.spv").string());
        VkShaderModule moon_frag = load_shader(device, (shader_dir / "satview_moon.frag.spv").string());
        VkShaderModule sun_vert = load_shader(device, (shader_dir / "satview_sun.vert.spv").string());
        VkShaderModule sun_frag = load_shader(device, (shader_dir / "satview_sun.frag.spv").string());
        VkShaderModule body_vert = load_shader(device, (shader_dir / "satview_body.vert.spv").string());
        VkShaderModule body_frag = load_shader(device, (shader_dir / "satview_body.frag.spv").string());
        VkShaderModule context_body_vert = load_shader(device, (shader_dir / "satview_context_body.vert.spv").string());
        VkShaderModule context_body_frag = load_shader(device, (shader_dir / "satview_context_body.frag.spv").string());
        VkShaderModule solid_body_frag = load_shader(device, (shader_dir / "satview_solid_body.frag.spv").string());
        VkShaderModule ring_vert = load_shader(device, (shader_dir / "satview_ring.vert.spv").string());
        VkShaderModule ring_frag = load_shader(device, (shader_dir / "satview_ring.frag.spv").string());
        VkShaderModule cloud_vert = load_shader(device, (shader_dir / "satview_cloud.vert.spv").string());
        VkShaderModule cloud_frag = load_shader(device, (shader_dir / "satview_cloud.frag.spv").string());
        VkShaderModule atmosphere_vert = load_shader(device, (shader_dir / "satview_atmosphere.vert.spv").string());
        VkShaderModule atmosphere_frag = load_shader(device, (shader_dir / "satview_atmosphere.frag.spv").string());
        VkShaderModule ground_atmosphere_vert = load_shader(device, (shader_dir / "satview_ground_atmosphere.vert.spv").string());
        VkShaderModule ground_atmosphere_frag = load_shader(device, (shader_dir / "satview_ground_atmosphere.frag.spv").string());
        VkShaderModule ground_surface_frag = load_shader(device, (shader_dir / "satview_ground_surface.frag.spv").string());
        VkShaderModule skybox_frag = load_shader(device, (shader_dir / "satview_skybox.frag.spv").string());
        VkShaderModule star_vert = load_shader(device, (shader_dir / "satview_star.vert.spv").string());
        VkShaderModule star_frag = load_shader(device, (shader_dir / "satview_star.frag.spv").string());
        VkShaderModule celestial_line_vert = load_shader(device, (shader_dir / "satview_celestial_line.vert.spv").string());
        VkShaderModule celestial_line_frag = load_shader(device, (shader_dir / "satview_celestial_line.frag.spv").string());
        VkShaderModule label_vert = load_shader(device, (shader_dir / "satview_label.vert.spv").string());
        VkShaderModule label_frag = load_shader(device, (shader_dir / "satview_label.frag.spv").string());
        VkShaderModule landscape_vert = load_shader(device, (shader_dir / "satview_landscape.vert.spv").string());
        VkShaderModule landscape_rim_vert = load_shader(device, (shader_dir / "satview_landscape_rim.vert.spv").string());
        VkShaderModule orbit_vert = load_shader(device, (shader_dir / "satview_orbit.vert.spv").string());
        VkShaderModule marker_vert = load_shader(device, (shader_dir / "satview_marker.vert.spv").string(), false);
        VkShaderModule orbit_frag = load_shader(device, (shader_dir / "satview_orbit.frag.spv").string());
        if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE
            || moon_vert == VK_NULL_HANDLE || moon_frag == VK_NULL_HANDLE
            || sun_vert == VK_NULL_HANDLE || sun_frag == VK_NULL_HANDLE
            || body_vert == VK_NULL_HANDLE || body_frag == VK_NULL_HANDLE
            || context_body_vert == VK_NULL_HANDLE || context_body_frag == VK_NULL_HANDLE
            || solid_body_frag == VK_NULL_HANDLE
            || ring_vert == VK_NULL_HANDLE || ring_frag == VK_NULL_HANDLE
            || cloud_vert == VK_NULL_HANDLE || cloud_frag == VK_NULL_HANDLE
            || atmosphere_vert == VK_NULL_HANDLE || atmosphere_frag == VK_NULL_HANDLE
            || ground_atmosphere_vert == VK_NULL_HANDLE || ground_atmosphere_frag == VK_NULL_HANDLE
            || ground_surface_frag == VK_NULL_HANDLE || skybox_frag == VK_NULL_HANDLE
            || star_vert == VK_NULL_HANDLE || star_frag == VK_NULL_HANDLE
            || celestial_line_vert == VK_NULL_HANDLE || celestial_line_frag == VK_NULL_HANDLE
            || label_vert == VK_NULL_HANDLE || label_frag == VK_NULL_HANDLE
            || landscape_vert == VK_NULL_HANDLE || landscape_rim_vert == VK_NULL_HANDLE
            || orbit_vert == VK_NULL_HANDLE || orbit_frag == VK_NULL_HANDLE)
        {
            if (vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, vert, nullptr);
            if (frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, frag, nullptr);
            if (moon_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, moon_vert, nullptr);
            if (moon_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, moon_frag, nullptr);
            if (sun_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, sun_vert, nullptr);
            if (sun_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, sun_frag, nullptr);
            if (body_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, body_vert, nullptr);
            if (body_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, body_frag, nullptr);
            if (context_body_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, context_body_vert, nullptr);
            if (context_body_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, context_body_frag, nullptr);
            if (solid_body_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, solid_body_frag, nullptr);
            if (ring_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, ring_vert, nullptr);
            if (ring_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, ring_frag, nullptr);
            if (cloud_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, cloud_vert, nullptr);
            if (cloud_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, cloud_frag, nullptr);
            if (atmosphere_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, atmosphere_vert, nullptr);
            if (atmosphere_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, atmosphere_frag, nullptr);
            if (ground_atmosphere_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, ground_atmosphere_vert, nullptr);
            if (ground_atmosphere_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, ground_atmosphere_frag, nullptr);
            if (ground_surface_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, ground_surface_frag, nullptr);
            if (skybox_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, skybox_frag, nullptr);
            if (star_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, star_vert, nullptr);
            if (star_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, star_frag, nullptr);
            if (celestial_line_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, celestial_line_vert, nullptr);
            if (celestial_line_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, celestial_line_frag, nullptr);
            if (label_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, label_vert, nullptr);
            if (label_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, label_frag, nullptr);
            if (landscape_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, landscape_vert, nullptr);
            if (landscape_rim_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, landscape_rim_vert, nullptr);
            if (orbit_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, orbit_vert, nullptr);
            if (marker_vert != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, marker_vert, nullptr);
            if (orbit_frag != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, orbit_frag, nullptr);
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertex_input{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

        VkPipelineInputAssemblyStateCreateInfo input_assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport_state{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = scene_sample_count;

        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;

        VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;

        VkGraphicsPipelineCreateInfo pipeline_ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipeline_ci.stageCount = 2;
        pipeline_ci.pStages = stages;
        pipeline_ci.pVertexInputState = &vertex_input;
        pipeline_ci.pInputAssemblyState = &input_assembly;
        pipeline_ci.pViewportState = &viewport_state;
        pipeline_ci.pRasterizationState = &raster;
        pipeline_ci.pMultisampleState = &multisample;
        pipeline_ci.pDepthStencilState = &depth;
        pipeline_ci.pColorBlendState = &blend;
        pipeline_ci.pDynamicState = &dynamic;
        pipeline_ci.layout = layout;
        pipeline_ci.renderPass = render_pass;
        pipeline_ci.subpass = 0;

        stages[0].module = ground_atmosphere_vert;
        stages[1].module = skybox_frag;
        depth.depthTestEnable = VK_FALSE;
        depth.depthWriteEnable = VK_FALSE;
        VkResult result = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &skybox_pipeline);
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        stages[0].module = vert;
        stages[1].module = frag;
        if (result == VK_SUCCESS)
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &earth_pipeline);
        if (result == VK_SUCCESS)
        {
            stages[0].module = moon_vert;
            stages[1].module = moon_frag;
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &moon_pipeline);
        }
        if (result == VK_SUCCESS)
        {
            stages[0].module = sun_vert;
            stages[1].module = sun_frag;
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &sun_pipeline);
        }
        if (result == VK_SUCCESS)
        {
            stages[0].module = body_vert;
            stages[1].module = body_frag;
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &body_pipeline);
        }
        if (result == VK_SUCCESS)
        {
            stages[0].module = context_body_vert;
            stages[1].module = context_body_frag;
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &context_body_pipeline);
        }
        if (result == VK_SUCCESS)
        {
            stages[0].module = context_body_vert;
            stages[1].module = solid_body_frag;
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &solid_body_pipeline);
        }
        if (result == VK_SUCCESS)
        {
            stages[0].module = ring_vert;
            stages[1].module = ring_frag;
            blend_attachment.blendEnable = VK_FALSE;
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &ring_pipeline);
        }
        if (result == VK_SUCCESS)
        {
            stages[0].module = cloud_vert;
            stages[1].module = cloud_frag;
            depth.depthWriteEnable = VK_FALSE;
            blend_attachment.blendEnable = VK_TRUE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &cloud_pipeline);
        }
        if (result == VK_SUCCESS)
        {
            stages[0].module = atmosphere_vert;
            stages[1].module = atmosphere_frag;
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &atmosphere_pipeline);
        }
        if (result == VK_SUCCESS)
        {
            stages[0].module = ground_atmosphere_vert;
            stages[1].module = ground_atmosphere_frag;
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &ground_atmosphere_pipeline);
        }
        if (result == VK_SUCCESS)
        {
            stages[0].module = ground_atmosphere_vert;
            stages[1].module = ground_surface_frag;
            depth.depthTestEnable = VK_FALSE;
            depth.depthWriteEnable = VK_FALSE;
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &ground_surface_pipeline);
            depth.depthTestEnable = VK_TRUE;
        }
        if (result == VK_SUCCESS)
        {
            VkVertexInputBindingDescription star_binding{};
            star_binding.binding = 0;
            star_binding.stride = sizeof(SatViewStarInstance);
            star_binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

            VkVertexInputAttributeDescription star_attributes[2]{};
            star_attributes[0].binding = 0;
            star_attributes[0].location = 0;
            star_attributes[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            star_attributes[0].offset = offsetof(SatViewStarInstance, direction_magnitude);
            star_attributes[1].binding = 0;
            star_attributes[1].location = 1;
            star_attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            star_attributes[1].offset = offsetof(SatViewStarInstance, color_size);

            vertex_input.vertexBindingDescriptionCount = 1;
            vertex_input.pVertexBindingDescriptions = &star_binding;
            vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(std::size(star_attributes));
            vertex_input.pVertexAttributeDescriptions = star_attributes;

            stages[0].module = star_vert;
            stages[1].module = star_frag;
            input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            depth.depthTestEnable = VK_FALSE;
            depth.depthWriteEnable = VK_FALSE;
            blend_attachment.blendEnable = VK_TRUE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &star_pipeline);
            depth.depthTestEnable = VK_TRUE;
        }
        if (result == VK_SUCCESS)
        {
            VkVertexInputBindingDescription line_binding{};
            line_binding.binding = 0;
            line_binding.stride = sizeof(SatViewCelestialLineInstance);
            line_binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
            VkVertexInputAttributeDescription line_attributes[4]{};
            for (uint32_t index = 0; index < 4; ++index)
            {
                line_attributes[index].binding = 0;
                line_attributes[index].location = index;
                line_attributes[index].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            }
            line_attributes[0].offset = offsetof(SatViewCelestialLineInstance, start_direction_width);
            line_attributes[1].offset = offsetof(SatViewCelestialLineInstance, end_direction_dash);
            line_attributes[2].offset = offsetof(SatViewCelestialLineInstance, color);
            line_attributes[3].offset = offsetof(SatViewCelestialLineInstance, style);
            vertex_input.vertexBindingDescriptionCount = 1;
            vertex_input.pVertexBindingDescriptions = &line_binding;
            vertex_input.vertexAttributeDescriptionCount = 4;
            vertex_input.pVertexAttributeDescriptions = line_attributes;
            stages[0].module = celestial_line_vert;
            stages[1].module = celestial_line_frag;
            input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            depth.depthTestEnable = VK_FALSE;
            depth.depthWriteEnable = VK_FALSE;
            blend_attachment.blendEnable = VK_TRUE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &constellation_pipeline);
        }
        if (result == VK_SUCCESS)
        {
            VkVertexInputBindingDescription orbit_binding{};
            orbit_binding.binding = 0;
            orbit_binding.stride = sizeof(SatViewSceneVertex);
            orbit_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            VkVertexInputAttributeDescription orbit_attributes[3]{};
            orbit_attributes[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                static_cast<uint32_t>(offsetof(SatViewSceneVertex, position)) };
            orbit_attributes[1] = { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                static_cast<uint32_t>(offsetof(SatViewSceneVertex, color)) };
            orbit_attributes[2] = { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                static_cast<uint32_t>(offsetof(SatViewSceneVertex, paired_position)) };
            vertex_input.vertexBindingDescriptionCount = 1;
            vertex_input.pVertexBindingDescriptions = &orbit_binding;
            vertex_input.vertexAttributeDescriptionCount = 3;
            vertex_input.pVertexAttributeDescriptions = orbit_attributes;
            stages[0].module = orbit_vert;
            stages[1].module = orbit_frag;
            input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            depth.depthTestEnable = VK_TRUE;
            depth.depthWriteEnable = VK_FALSE;
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &orbit_pipeline);
        }
        if (result == VK_SUCCESS)
        {
            VkVertexInputBindingDescription landscape_binding{};
            landscape_binding.binding = 0;
            landscape_binding.stride = sizeof(SatViewLandscapeTriangleInstance);
            landscape_binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
            VkVertexInputAttributeDescription landscape_attributes[4]{};
            for (uint32_t index = 0; index < 4; ++index)
            {
                landscape_attributes[index].binding = 0;
                landscape_attributes[index].location = index;
                landscape_attributes[index].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            }
            landscape_attributes[0].offset = offsetof(SatViewLandscapeTriangleInstance, local_direction0);
            landscape_attributes[1].offset = offsetof(SatViewLandscapeTriangleInstance, local_direction1);
            landscape_attributes[2].offset = offsetof(SatViewLandscapeTriangleInstance, local_direction2);
            landscape_attributes[3].offset = offsetof(SatViewLandscapeTriangleInstance, color);
            vertex_input.vertexBindingDescriptionCount = 1;
            vertex_input.pVertexBindingDescriptions = &landscape_binding;
            vertex_input.vertexAttributeDescriptionCount = 4;
            vertex_input.pVertexAttributeDescriptions = landscape_attributes;
            stages[0].module = landscape_vert;
            stages[1].module = orbit_frag;
            depth.depthTestEnable = VK_FALSE;
            depth.depthWriteEnable = VK_FALSE;
            blend_attachment.blendEnable = VK_TRUE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &landscape_fill_pipeline);
            if (result == VK_SUCCESS)
            {
                VkVertexInputBindingDescription rim_binding{};
                rim_binding.binding = 0;
                rim_binding.stride = sizeof(SatViewLandscapeLineInstance);
                rim_binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
                VkVertexInputAttributeDescription rim_attributes[3]{};
                for (uint32_t index = 0; index < 3; ++index)
                {
                    rim_attributes[index].binding = 0;
                    rim_attributes[index].location = index;
                    rim_attributes[index].format = VK_FORMAT_R32G32B32A32_SFLOAT;
                }
                rim_attributes[0].offset = offsetof(SatViewLandscapeLineInstance, local_direction0);
                rim_attributes[1].offset = offsetof(SatViewLandscapeLineInstance, local_direction1);
                rim_attributes[2].offset = offsetof(SatViewLandscapeLineInstance, color);
                vertex_input.pVertexBindingDescriptions = &rim_binding;
                vertex_input.vertexAttributeDescriptionCount = 3;
                vertex_input.pVertexAttributeDescriptions = rim_attributes;
                stages[0].module = landscape_rim_vert;
                input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
                result = vkCreateGraphicsPipelines(
                    device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &landscape_rim_pipeline);
            }
        }
        if (result == VK_SUCCESS)
        {
            VkVertexInputBindingDescription label_binding{};
            label_binding.binding = 0;
            label_binding.stride = sizeof(SatViewLabelInstance);
            label_binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
            VkVertexInputAttributeDescription label_attributes[4]{};
            for (uint32_t index = 0; index < 4; ++index)
            {
                label_attributes[index].binding = 0;
                label_attributes[index].location = index;
                label_attributes[index].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            }
            label_attributes[0].offset = offsetof(SatViewLabelInstance, direction_priority);
            label_attributes[1].offset = offsetof(SatViewLabelInstance, uv_rect);
            label_attributes[2].offset = offsetof(SatViewLabelInstance, pixel_size_offset);
            label_attributes[3].offset = offsetof(SatViewLabelInstance, color);
            vertex_input.vertexBindingDescriptionCount = 1;
            vertex_input.pVertexBindingDescriptions = &label_binding;
            vertex_input.vertexAttributeDescriptionCount = 4;
            vertex_input.pVertexAttributeDescriptions = label_attributes;
            stages[0].module = label_vert;
            stages[1].module = label_frag;
            input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            depth.depthTestEnable = VK_FALSE;
            depth.depthWriteEnable = VK_FALSE;
            blend_attachment.blendEnable = VK_TRUE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            result = vkCreateGraphicsPipelines(
                device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &label_pipeline);
        }
        if (result == VK_SUCCESS && marker_vert != VK_NULL_HANDLE)
        {
            VkVertexInputBindingDescription marker_binding{};
            marker_binding.binding = 0;
            marker_binding.stride = sizeof(SatViewMarkerInstance);
            marker_binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

            VkVertexInputAttributeDescription marker_attributes[5]{};
            marker_attributes[0].binding = 0;
            marker_attributes[0].location = 0;
            marker_attributes[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            marker_attributes[0].offset = offsetof(SatViewMarkerInstance, position0_size);
            marker_attributes[1].binding = 0;
            marker_attributes[1].location = 1;
            marker_attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            marker_attributes[1].offset = offsetof(SatViewMarkerInstance, position1_selected);
            marker_attributes[2].binding = 0;
            marker_attributes[2].location = 2;
            marker_attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            marker_attributes[2].offset = offsetof(SatViewMarkerInstance, color);
            marker_attributes[3].binding = 0;
            marker_attributes[3].location = 3;
            marker_attributes[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            marker_attributes[3].offset = offsetof(SatViewMarkerInstance, style);
            marker_attributes[4].binding = 0;
            marker_attributes[4].location = 4;
            marker_attributes[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            marker_attributes[4].offset = offsetof(SatViewMarkerInstance, surface_normal);

            vertex_input.vertexBindingDescriptionCount = 1;
            vertex_input.pVertexBindingDescriptions = &marker_binding;
            vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(std::size(marker_attributes));
            vertex_input.pVertexAttributeDescriptions = marker_attributes;

            stages[0].module = marker_vert;
            stages[1].module = orbit_frag;
            input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            depth.depthTestEnable = VK_TRUE;
            depth.depthWriteEnable = VK_FALSE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            const VkResult marker_result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &marker_pipeline);
            if (marker_result != VK_SUCCESS)
            {
                marker_pipeline = VK_NULL_HANDLE;
                DRAXUL_LOG_WARN(LogCategory::Renderer, "SatView: failed to create optional marker pipeline");
            }
        }

        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        vkDestroyShaderModule(device, moon_vert, nullptr);
        vkDestroyShaderModule(device, moon_frag, nullptr);
        vkDestroyShaderModule(device, sun_vert, nullptr);
        vkDestroyShaderModule(device, sun_frag, nullptr);
        vkDestroyShaderModule(device, body_vert, nullptr);
        vkDestroyShaderModule(device, body_frag, nullptr);
        vkDestroyShaderModule(device, context_body_vert, nullptr);
        vkDestroyShaderModule(device, context_body_frag, nullptr);
        vkDestroyShaderModule(device, solid_body_frag, nullptr);
        vkDestroyShaderModule(device, ring_vert, nullptr);
        vkDestroyShaderModule(device, ring_frag, nullptr);
        vkDestroyShaderModule(device, cloud_vert, nullptr);
        vkDestroyShaderModule(device, cloud_frag, nullptr);
        vkDestroyShaderModule(device, atmosphere_vert, nullptr);
        vkDestroyShaderModule(device, atmosphere_frag, nullptr);
        vkDestroyShaderModule(device, ground_atmosphere_vert, nullptr);
        vkDestroyShaderModule(device, ground_atmosphere_frag, nullptr);
        vkDestroyShaderModule(device, ground_surface_frag, nullptr);
        vkDestroyShaderModule(device, skybox_frag, nullptr);
        vkDestroyShaderModule(device, star_vert, nullptr);
        vkDestroyShaderModule(device, star_frag, nullptr);
        vkDestroyShaderModule(device, celestial_line_vert, nullptr);
        vkDestroyShaderModule(device, celestial_line_frag, nullptr);
        vkDestroyShaderModule(device, label_vert, nullptr);
        vkDestroyShaderModule(device, label_frag, nullptr);
        vkDestroyShaderModule(device, landscape_vert, nullptr);
        vkDestroyShaderModule(device, landscape_rim_vert, nullptr);
        vkDestroyShaderModule(device, orbit_vert, nullptr);
        if (marker_vert != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, marker_vert, nullptr);
        vkDestroyShaderModule(device, orbit_frag, nullptr);
        if (result != VK_SUCCESS)
        {
            DRAXUL_LOG_ERROR(LogCategory::Renderer, "SatView: failed to create graphics pipeline");
            destroy_pipelines();
            return false;
        }
        return true;
    }
};

SatViewScenePass::SatViewScenePass()
    : state_(std::make_unique<State>())
{
}

SatViewScenePass::~SatViewScenePass() = default;

void SatViewScenePass::record_prepass(IRenderContext& ctx)
{
    PERF_MEASURE();
    auto* vk_ctx = static_cast<VkRenderContext*>(&ctx);
    const int width = std::max(1, ctx.viewport_w());
    const int height = std::max(1, ctx.viewport_h());
    if (!state_->ensure_texture_descriptors(*vk_ctx)
        || !state_->ensure_hdr_setup(*vk_ctx)
        || !state_->ensure_hdr_targets(vk_ctx->buffered_frame_count(), width, height))
        return;

    if (pending_cloud_image_
        && state_->ensure_cloud_texture(*vk_ctx, *pending_cloud_image_, cloud_revision_))
        pending_cloud_image_.reset();
    if (focus_body_enabled_)
        state_->ensure_focus_body_texture(*vk_ctx, focus_body_);
    if (context_body_enabled_)
        state_->ensure_context_body_texture(*vk_ctx, context_body_);
    if (label_atlas_)
        state_->ensure_label_texture(*vk_ctx, *label_atlas_, label_atlas_revision_);
    state_->ensure_vertex_buffer(
        *vk_ctx,
        track_vertices_,
        track_revision_,
        state_->track_vertex_buffer,
        state_->track_vertex_count,
        state_->uploaded_track_revision);
    state_->ensure_vertex_buffer(
        *vk_ctx,
        earth_track_vertices_,
        earth_track_revision_,
        state_->earth_track_vertex_buffer,
        state_->earth_track_vertex_count,
        state_->uploaded_earth_track_revision);
    state_->ensure_vertex_buffer(
        *vk_ctx,
        markers_,
        marker_revision_,
        state_->marker_buffer,
        state_->marker_count,
        state_->uploaded_marker_revision);
    state_->ensure_vertex_buffer(
        *vk_ctx,
        surface_markers_,
        surface_marker_revision_,
        state_->surface_marker_buffer,
        state_->surface_marker_count,
        state_->uploaded_surface_marker_revision);
    state_->ensure_vertex_buffer(
        *vk_ctx,
        stars_,
        star_revision_,
        state_->star_buffer,
        state_->star_count,
        state_->uploaded_star_revision);
    state_->ensure_vertex_buffer(
        *vk_ctx,
        constellation_lines_,
        constellation_revision_,
        state_->constellation_buffer,
        state_->constellation_line_count,
        state_->uploaded_constellation_revision);
    state_->ensure_vertex_buffer(
        *vk_ctx,
        constellation_boundary_lines_,
        constellation_boundary_revision_,
        state_->constellation_boundary_buffer,
        state_->constellation_boundary_line_count,
        state_->uploaded_constellation_boundary_revision);
    state_->ensure_vertex_buffer(
        *vk_ctx,
        constellation_labels_,
        constellation_label_revision_,
        state_->constellation_label_buffer,
        state_->constellation_label_count,
        state_->uploaded_constellation_label_revision);
    state_->ensure_vertex_buffer(
        *vk_ctx,
        cardinal_labels_,
        cardinal_label_revision_,
        state_->cardinal_label_buffer,
        state_->cardinal_label_count,
        state_->uploaded_cardinal_label_revision);
    state_->ensure_vertex_buffer(
        *vk_ctx,
        observatory_fill_triangles_,
        observatory_revision_,
        state_->observatory_fill_buffer,
        state_->observatory_fill_triangle_count,
        state_->uploaded_observatory_revision);
    state_->ensure_vertex_buffer(
        *vk_ctx,
        observatory_rim_lines_,
        observatory_revision_,
        state_->observatory_rim_buffer,
        state_->observatory_rim_line_count,
        state_->uploaded_observatory_rim_revision);

    const uint32_t frame_index = vk_ctx->frame_index() % static_cast<uint32_t>(state_->hdr_targets.size());
    auto& targets = state_->hdr_targets[frame_index];
    VkCommandBuffer cmd = vk_ctx->command_buffer();

    std::array<VkClearValue, 2> scene_clear{};
    scene_clear[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    scene_clear[1].depthStencil = { 0.0f, 0 };
    VkRenderPassBeginInfo scene_begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    scene_begin.renderPass = hdr_debug_enabled_ && state_->scene_debug_render_pass != VK_NULL_HANDLE
        ? state_->scene_debug_render_pass
        : state_->scene_render_pass;
    scene_begin.framebuffer = targets.shared.scene_framebuffer;
    scene_begin.renderArea.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    scene_begin.clearValueCount = static_cast<uint32_t>(scene_clear.size());
    scene_begin.pClearValues = scene_clear.data();
    vkCmdBeginRenderPass(cmd, &scene_begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport local_viewport{};
    local_viewport.width = static_cast<float>(width);
    local_viewport.height = static_cast<float>(height);
    local_viewport.maxDepth = 1.0f;
    VkRect2D local_scissor{};
    local_scissor.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    vkCmdSetViewport(cmd, 0, 1, &local_viewport);
    vkCmdSetScissor(cmd, 0, 1, &local_scissor);

    SatViewFrameUniforms frame = frame_;
    frame.view_proj = make_satview_vulkan_clip_matrix(frame.view_proj);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->layout,
        0, 1, &state_->descriptor_set, 0, nullptr);
    auto draw_stars = [&]() {
        const uint32_t draw_count = state_->star_count;
        if (draw_count == 0
            || state_->star_buffer.buffer == VK_NULL_HANDLE
            || state_->star_pipeline == VK_NULL_HANDLE)
            return;

        VkDeviceSize offset = 0;
        SatViewFrameUniforms star_frame = frame;
        star_frame.render_params.x = star_min_magnitude_;
        star_frame.render_params.y = star_max_magnitude_;
        star_frame.render_params.z = star_projection_aspect_scale_;
        star_frame.render_params.w = star_brightness_scale_;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->star_pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &state_->star_buffer.buffer, &offset);
        vkCmdPushConstants(cmd, state_->layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SatViewFrameUniforms), &star_frame);
        vkCmdDraw(cmd, kSatViewStarVerticesPerInstance, draw_count, 0, 0);
    };
    auto draw_constellations = [&]() {
        if (!constellation_lines_enabled_
            || state_->constellation_line_count == 0
            || state_->constellation_buffer.buffer == VK_NULL_HANDLE
            || state_->constellation_pipeline == VK_NULL_HANDLE)
            return;

        VkDeviceSize offset = 0;
        SatViewFrameUniforms line_frame = frame;
        line_frame.render_params = glm::vec4(
            static_cast<float>(width), static_cast<float>(height), 0.0f, 0.0f);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->constellation_pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &state_->constellation_buffer.buffer, &offset);
        vkCmdPushConstants(cmd, state_->layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SatViewFrameUniforms), &line_frame);
        vkCmdDraw(cmd, 6, state_->constellation_line_count, 0, 0);
    };
    auto draw_boundaries = [&]() {
        if (!constellation_boundaries_enabled_
            || state_->constellation_boundary_line_count == 0
            || state_->constellation_boundary_buffer.buffer == VK_NULL_HANDLE
            || state_->constellation_pipeline == VK_NULL_HANDLE)
            return;
        VkDeviceSize offset = 0;
        SatViewFrameUniforms line_frame = frame;
        line_frame.render_params = glm::vec4(
            static_cast<float>(width), static_cast<float>(height), 0.0f, 0.0f);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->constellation_pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &state_->constellation_boundary_buffer.buffer, &offset);
        vkCmdPushConstants(cmd, state_->layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SatViewFrameUniforms), &line_frame);
        vkCmdDraw(cmd, 6, state_->constellation_boundary_line_count, 0, 0);
    };
    auto draw_labels = [&](BufferResource& buffer, uint32_t count) {
        if (count == 0 || buffer.buffer == VK_NULL_HANDLE || state_->label_pipeline == VK_NULL_HANDLE)
            return;
        VkDeviceSize offset = 0;
        SatViewFrameUniforms label_frame = frame;
        label_frame.render_params = glm::vec4(
            static_cast<float>(width), static_cast<float>(height), 0.0f, 0.0f);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->label_pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &buffer.buffer, &offset);
        vkCmdPushConstants(cmd, state_->layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SatViewFrameUniforms), &label_frame);
        vkCmdDraw(cmd, 6, count, 0, 0);
    };
    if (milky_way_enabled_ && !map_projection_)
    {
        SatViewFrameUniforms skybox_frame = frame;
        skybox_frame.render_params.w = milky_way_brightness_;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->skybox_pipeline);
        vkCmdPushConstants(cmd, state_->layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SatViewFrameUniforms), &skybox_frame);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    SatViewFrameUniforms focus_frame = frame;
    focus_frame.render_params.z = focus_body_rotation_radians_;
    focus_frame.render_params.w = focus_body_polar_radius_ratio_;
    focus_frame.sun_dir_time.w = focus_body_emissive_ ? -1.0f : 0.0f;
    if (map_projection_)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            focus_body_enabled_        ? state_->body_pipeline
                : sun_map_projection_  ? state_->sun_pipeline
                : moon_map_projection_ ? state_->moon_pipeline
                                       : state_->earth_pipeline);
        vkCmdPushConstants(cmd, state_->layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SatViewFrameUniforms), focus_body_enabled_ ? &focus_frame : &frame);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }
    else if (ground_projection_)
    {
        if (atmosphere_enabled_)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->ground_atmosphere_pipeline);
            vkCmdPushConstants(cmd, state_->layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(SatViewFrameUniforms), &frame);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }

        draw_boundaries();
        draw_constellations();
        draw_stars();
        draw_labels(state_->constellation_label_buffer, state_->constellation_label_count);

        if (sun_enabled_ && sun_position_radius_.w > 0.0f)
        {
            SatViewFrameUniforms sun_frame = frame;
            sun_frame.camera_orientation = sun_position_radius_;
            sun_frame.sun_dir_time = glm::vec4(
                sun_body_to_render_.x,
                sun_body_to_render_.y,
                sun_body_to_render_.z,
                sun_body_to_render_.w);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->sun_pipeline);
            vkCmdPushConstants(cmd, state_->layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(SatViewFrameUniforms), &sun_frame);
            vkCmdDraw(cmd, kSatViewSphereVertexCount, 1, 0, 0);
        }

        if (moon_enabled_ && moon_position_radius_.w > 0.0f)
        {
            SatViewFrameUniforms moon_frame = frame;
            moon_frame.camera_orientation = moon_position_radius_;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->moon_pipeline);
            vkCmdPushConstants(cmd, state_->layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(SatViewFrameUniforms), &moon_frame);
            vkCmdDraw(cmd, kSatViewSphereVertexCount, 1, 0, 0);
        }

        vkCmdPushConstants(cmd, state_->layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SatViewFrameUniforms), &frame);
    }
    else
    {
        draw_boundaries();
        draw_constellations();
        draw_stars();
        draw_labels(state_->constellation_label_buffer, state_->constellation_label_count);

        if (sun_enabled_ && sun_position_radius_.w > 0.0f)
        {
            SatViewFrameUniforms sun_frame = frame;
            sun_frame.camera_orientation = sun_position_radius_;
            sun_frame.sun_dir_time = glm::vec4(
                sun_body_to_render_.x,
                sun_body_to_render_.y,
                sun_body_to_render_.z,
                sun_body_to_render_.w);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->sun_pipeline);
            vkCmdPushConstants(cmd, state_->layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(SatViewFrameUniforms), &sun_frame);
            vkCmdDraw(cmd, kSatViewSphereVertexCount, 1, 0, 0);
        }

        if (context_body_enabled_ && context_body_position_radius_.w > 0.0f)
        {
            SatViewFrameUniforms context_frame = frame;
            context_frame.camera_orientation = context_body_position_radius_;
            context_frame.render_params.z = context_body_rotation_radians_;
            context_frame.render_params.w = context_body_polar_radius_ratio_;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->context_body_pipeline);
            vkCmdPushConstants(cmd, state_->layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(SatViewFrameUniforms), &context_frame);
            vkCmdDraw(cmd, kSatViewSphereVertexCount, 1, 0, 0);
        }

        if (moon_enabled_ && moon_position_radius_.w > 0.0f)
        {
            SatViewFrameUniforms moon_frame = frame;
            moon_frame.camera_orientation = moon_position_radius_;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->moon_pipeline);
            vkCmdPushConstants(cmd, state_->layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(SatViewFrameUniforms), &moon_frame);
            vkCmdDraw(cmd, kSatViewSphereVertexCount, 1, 0, 0);
        }

        if (!ring_bands_.empty())
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->ring_pipeline);
            for (const SatViewRingBand& band : ring_bands_)
            {
                SatViewFrameUniforms ring_frame = frame;
                const float radius_scale = static_cast<float>(
                    std::max(band.radius_scale_focus_radii, 0.0));
                const float inner_radius = static_cast<float>(
                    band.inner_radius_body_radii * radius_scale);
                const float outer_radius = static_cast<float>(
                    band.outer_radius_body_radii * radius_scale);
                ring_frame.camera_orientation = glm::vec4(
                    glm::vec3(band.center_focus_radii),
                    outer_radius);
                ring_frame.render_params.x = inner_radius;
                ring_frame.sun_dir_time = band.color;
                vkCmdPushConstants(cmd, state_->layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(SatViewFrameUniforms), &ring_frame);
                vkCmdDraw(cmd, 6, 1, 0, 0);
            }
        }

        if (!child_bodies_.empty())
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->solid_body_pipeline);
            for (const SatViewBodyRenderInstance& body : child_bodies_)
            {
                SatViewFrameUniforms body_instance_frame = frame;
                body_instance_frame.camera_orientation = glm::vec4(
                    glm::vec3(body.position_focus_radii),
                    static_cast<float>(body.radius_focus_radii));
                body_instance_frame.render_params.z = static_cast<float>(body.rotation_radians);
                body_instance_frame.render_params.w = static_cast<float>(body.polar_radius_ratio);
                body_instance_frame.sun_dir_time.w = static_cast<float>(body.body);
                vkCmdPushConstants(cmd, state_->layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(SatViewFrameUniforms), &body_instance_frame);
                vkCmdDraw(cmd, kSatViewSphereVertexCount, 1, 0, 0);
            }
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            focus_body_enabled_ ? state_->body_pipeline : state_->earth_pipeline);
        vkCmdPushConstants(cmd, state_->layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SatViewFrameUniforms), focus_body_enabled_ ? &focus_frame : &frame);
        vkCmdDraw(cmd, kSatViewSphereVertexCount, 1, 0, 0);

        if (!focus_body_enabled_ && frame.sun_dir_time.w > 0.5f)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->cloud_pipeline);
            vkCmdDraw(cmd, kSatViewSphereVertexCount, 1, 0, 0);
        }

        if (!focus_body_enabled_ && atmosphere_enabled_)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->atmosphere_pipeline);
            vkCmdDraw(cmd, kSatViewSphereVertexCount, 1, 0, 0);
        }
    }

    if (state_->earth_track_vertex_count != 0
        && state_->earth_track_vertex_buffer.buffer != VK_NULL_HANDLE)
    {
        VkDeviceSize offset = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->orbit_pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &state_->earth_track_vertex_buffer.buffer, &offset);
        SatViewFrameUniforms earth_track_frame = frame;
        earth_track_frame.render_params.w = -1.0f;
        if (map_projection_)
        {
            for (int copy = -1; copy <= 1; ++copy)
            {
                earth_track_frame.camera_orientation.w = static_cast<float>(copy * 2);
                vkCmdPushConstants(cmd, state_->layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(SatViewFrameUniforms), &earth_track_frame);
                vkCmdDraw(cmd, state_->earth_track_vertex_count, 1, 0, 0);
            }
        }
        else
        {
            earth_track_frame.camera_orientation = sun_position_radius_;
            vkCmdPushConstants(cmd, state_->layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(SatViewFrameUniforms), &earth_track_frame);
            vkCmdDraw(cmd, state_->earth_track_vertex_count, 1, 0, 0);
        }
        vkCmdPushConstants(cmd, state_->layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SatViewFrameUniforms), &frame);
    }

    if (state_->track_vertex_count != 0 && state_->track_vertex_buffer.buffer != VK_NULL_HANDLE)
    {
        VkDeviceSize offset = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->orbit_pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &state_->track_vertex_buffer.buffer, &offset);
        if (map_projection_)
        {
            for (int copy = -1; copy <= 1; ++copy)
            {
                SatViewFrameUniforms track_frame = frame;
                track_frame.camera_orientation.w = static_cast<float>(copy * 2);
                vkCmdPushConstants(cmd, state_->layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(SatViewFrameUniforms), &track_frame);
                vkCmdDraw(cmd, state_->track_vertex_count, 1, 0, 0);
            }
            vkCmdPushConstants(cmd, state_->layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(SatViewFrameUniforms), &frame);
        }
        else
        {
            vkCmdDraw(cmd, state_->track_vertex_count, 1, 0, 0);
        }
    }

    if (state_->marker_count != 0
        && state_->marker_buffer.buffer != VK_NULL_HANDLE
        && state_->marker_pipeline != VK_NULL_HANDLE)
    {
        VkDeviceSize offset = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->marker_pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &state_->marker_buffer.buffer, &offset);
        vkCmdDraw(cmd, kSatViewMarkerVerticesPerInstance, state_->marker_count, 0, 0);
    }

    if (state_->surface_marker_count != 0
        && state_->surface_marker_buffer.buffer != VK_NULL_HANDLE
        && state_->marker_pipeline != VK_NULL_HANDLE)
    {
        VkDeviceSize offset = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->marker_pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &state_->surface_marker_buffer.buffer, &offset);
        vkCmdDraw(cmd, kSatViewMarkerVerticesPerInstance, state_->surface_marker_count, 0, 0);
    }

    if (ground_projection_ && ground_visible_)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->ground_surface_pipeline);
        vkCmdPushConstants(cmd, state_->layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(SatViewFrameUniforms), &frame);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    if (ground_projection_ && observatory_horizon_enabled_)
    {
        VkDeviceSize offset = 0;
        if (state_->observatory_fill_triangle_count != 0
            && state_->observatory_fill_buffer.buffer != VK_NULL_HANDLE)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->landscape_fill_pipeline);
            vkCmdBindVertexBuffers(cmd, 0, 1, &state_->observatory_fill_buffer.buffer, &offset);
            vkCmdPushConstants(cmd, state_->layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(SatViewFrameUniforms), &frame);
            vkCmdDraw(cmd, 3, state_->observatory_fill_triangle_count, 0, 0);
        }
        if (state_->observatory_rim_line_count != 0
            && state_->observatory_rim_buffer.buffer != VK_NULL_HANDLE)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->landscape_rim_pipeline);
            vkCmdBindVertexBuffers(cmd, 0, 1, &state_->observatory_rim_buffer.buffer, &offset);
            vkCmdDraw(cmd, 2, state_->observatory_rim_line_count, 0, 0);
        }
    }
    if (ground_projection_)
        draw_labels(state_->cardinal_label_buffer, state_->cardinal_label_count);

    vkCmdEndRenderPass(cmd);

    if (hdr_debug_enabled_)
    {
        VkClearValue debug_clear{};
        debug_clear.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
        VkRenderPassBeginInfo debug_begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        debug_begin.renderPass = state_->debug_render_pass;
        debug_begin.framebuffer = targets.debug_framebuffer;
        debug_begin.renderArea.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
        debug_begin.clearValueCount = 1;
        debug_begin.pClearValues = &debug_clear;
        vkCmdBeginRenderPass(cmd, &debug_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(cmd, 0, 1, &local_viewport);
        vkCmdSetScissor(cmd, 0, 1, &local_scissor);
        if (state_->scene_sample_count != VK_SAMPLE_COUNT_1_BIT
            && state_->msaa_debug_pipeline != VK_NULL_HANDLE
            && targets.debug_descriptor_set != VK_NULL_HANDLE)
        {
            const glm::ivec4 debug_params(static_cast<int>(sample_count_value(state_->scene_sample_count)), 0, 0, 0);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->msaa_debug_pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->debug_pipeline_layout,
                0, 1, &targets.debug_descriptor_set, 0, nullptr);
            vkCmdPushConstants(cmd, state_->debug_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(debug_params), &debug_params);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd);
        targets.debug_ready = true;
    }

    VkClearValue post_clear{};
    post_clear.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkRenderPassBeginInfo post_begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    post_begin.renderPass = state_->post_render_pass;
    post_begin.framebuffer = targets.shared.tone_map_framebuffer;
    post_begin.renderArea.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    post_begin.clearValueCount = 1;
    post_begin.pClearValues = &post_clear;
    vkCmdBeginRenderPass(cmd, &post_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(cmd, 0, 1, &local_viewport);
    vkCmdSetScissor(cmd, 0, 1, &local_scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->tone_map_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->post_pipeline_layout,
        0, 1, &targets.post_descriptor_set, 0, nullptr);
    const glm::vec4 tone_map(tone_map_exposure_, tone_map_white_point_, 0.0f, 0.0f);
    vkCmdPushConstants(cmd, state_->post_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(tone_map), &tone_map);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    state_->last_prepass_frame = frame_index;
}

void SatViewScenePass::record(IRenderContext& ctx)
{
    PERF_MEASURE();
    auto* vk_ctx = static_cast<VkRenderContext*>(&ctx);
    if (state_->hdr_targets.empty() || state_->present_pipeline == VK_NULL_HANDLE)
        return;
    const uint32_t frame_index = vk_ctx->frame_index() % static_cast<uint32_t>(state_->hdr_targets.size());
    const auto& targets = state_->hdr_targets[frame_index];
    if (targets.present_descriptor_set == VK_NULL_HANDLE)
        return;

    VkViewport viewport{};
    viewport.x = static_cast<float>(ctx.viewport_x());
    viewport.y = static_cast<float>(ctx.viewport_y());
    viewport.width = static_cast<float>(ctx.viewport_w());
    viewport.height = static_cast<float>(ctx.viewport_h());
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.offset = { std::max(0, ctx.viewport_x()), std::max(0, ctx.viewport_y()) };
    scissor.extent = {
        static_cast<uint32_t>(std::max(0, ctx.viewport_w())),
        static_cast<uint32_t>(std::max(0, ctx.viewport_h()))
    };
    VkCommandBuffer cmd = vk_ctx->command_buffer();
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->present_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state_->post_pipeline_layout,
        0, 1, &targets.present_descriptor_set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void SatViewScenePass::render_hdr_debug_ui()
{
    if (state_->hdr_targets.empty() || state_->hdr_sampler == VK_NULL_HANDLE)
        return;
    auto& targets = state_->hdr_targets[state_->last_prepass_frame % static_cast<uint32_t>(state_->hdr_targets.size())];

    if (targets.imgui_hdr_descriptor_set == VK_NULL_HANDLE)
    {
        targets.imgui_hdr_descriptor_set = ImGui_ImplVulkan_AddTexture(
            state_->hdr_sampler, targets.shared.scene_hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (targets.imgui_final_descriptor_set == VK_NULL_HANDLE)
    {
        targets.imgui_final_descriptor_set = ImGui_ImplVulkan_AddTexture(
            state_->hdr_sampler, targets.shared.scene_final_unorm_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (targets.debug_ready && targets.imgui_msaa_descriptor_set == VK_NULL_HANDLE)
    {
        targets.imgui_msaa_descriptor_set = ImGui_ImplVulkan_AddTexture(
            state_->hdr_sampler, targets.msaa_difference.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    if (!ImGui::Begin("SatView HDR Buffers"))
    {
        ImGui::End();
        return;
    }
    const uint32_t samples = sample_count_value(state_->scene_sample_count);
    ImGui::Text("Requested: 4x  Active: %ux%s", samples, samples < 4 ? " (fallback)" : "");
    ImGui::Text("Size: %dx%d  Exposure: %.2f  White point: %.2f",
        targets.width(), targets.height(), tone_map_exposure_, tone_map_white_point_);

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float aspect = targets.height() > 0
        ? static_cast<float>(targets.width()) / static_cast<float>(targets.height())
        : 1.0f;
    const float width = std::max(64.0f, (available.x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    const ImVec2 size(width, width / aspect);
    if (ImGui::BeginTable("##satview_hdr_grid", 2))
    {
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("MSAA Difference");
        if (targets.imgui_msaa_descriptor_set != VK_NULL_HANDLE)
            ImGui::Image(static_cast<ImTextureID>(targets.imgui_msaa_descriptor_set), size);
        else
            ImGui::TextDisabled("Waiting for debug frame");

        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Scene HDR Resolved");
        if (targets.imgui_hdr_descriptor_set != VK_NULL_HANDLE)
            ImGui::Image(static_cast<ImTextureID>(targets.imgui_hdr_descriptor_set), size);

        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Scene Final");
        if (targets.imgui_final_descriptor_set != VK_NULL_HANDLE)
        {
            ImGui::Image(static_cast<ImTextureID>(targets.imgui_final_descriptor_set),
                size, ImVec2(0, 1), ImVec2(1, 0));
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace draxul::satview
