#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

std::string read_text_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::string text(std::istreambuf_iterator<char>(in), {});
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

std::string source_block(
    const std::string& source,
    std::string_view begin_marker,
    std::string_view end_marker)
{
    const std::size_t begin = source.find(begin_marker);
    if (begin == std::string::npos)
        return {};
    const std::size_t end = source.find(end_marker, begin);
    if (end == std::string::npos)
        return source.substr(begin);
    return source.substr(begin, end - begin);
}

} // namespace

TEST_CASE("SatView rings use opaque depth-writing render state", "[satview][rings][shader]")
{
    const auto metal_source = read_text_file(
        std::filesystem::path(DRAXUL_PROJECT_ROOT)
        / "plugins" / "satview" / "src" / "render" / "satview_render.mm");
    REQUIRE(!metal_source.empty());

    const std::string metal_ring_pipeline = source_block(
        metal_source,
        "desc.vertexFunction = ring_vertex;",
        "ring_pipeline.reset(created);");
    REQUIRE_FALSE(metal_ring_pipeline.empty());
    CHECK(metal_ring_pipeline.find("blendingEnabled = NO") != std::string::npos);
    CHECK(metal_ring_pipeline.find("blendingEnabled = YES") == std::string::npos);
    CHECK(metal_source.find("[encoder setDepthStencilState:state_->depth_write_state.get()];\n"
                            "            for (const SatViewRingBand& band : ring_bands_)")
        != std::string::npos);

    const auto vulkan_source = read_text_file(
        std::filesystem::path(DRAXUL_PROJECT_ROOT)
        / "plugins" / "satview" / "src" / "render" / "satview_render_vk.cpp");
    REQUIRE(!vulkan_source.empty());

    const std::string vulkan_ring_pipeline = source_block(
        vulkan_source,
        "stages[0].module = ring_vert;",
        "VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &ring_pipeline);");
    REQUIRE_FALSE(vulkan_ring_pipeline.empty());
    CHECK(vulkan_ring_pipeline.find("blend_attachment.blendEnable = VK_TRUE") == std::string::npos);
    CHECK(vulkan_ring_pipeline.find("depth.depthWriteEnable = VK_FALSE") == std::string::npos);
}

TEST_CASE("SatView Vulkan streams remain owned by their buffered frame slot",
    "[satview][vulkan][streams]")
{
    const auto source = read_text_file(
        std::filesystem::path(DRAXUL_PROJECT_ROOT)
        / "plugins" / "satview" / "src" / "render" / "satview_render_vk.cpp");
    REQUIRE_FALSE(source.empty());

    const std::string frame_streams = source_block(
        source,
        "struct FrameStreams",
        "uint64_t uploaded_label_atlas_revision");
    REQUIRE_FALSE(frame_streams.empty());
    CHECK(frame_streams.find("BufferResource track_vertex_buffer") != std::string::npos);
    CHECK(frame_streams.find("BufferResource marker_buffer") != std::string::npos);
    CHECK(frame_streams.find("BufferResource surface_marker_buffer") != std::string::npos);
    CHECK(frame_streams.find("uint64_t uploaded_track_revision") != std::string::npos);
    CHECK(frame_streams.find("uint64_t uploaded_marker_revision") != std::string::npos);
    CHECK(frame_streams.find("std::vector<FrameStreams> frame_streams") != std::string::npos);

    const std::string prepass = source_block(
        source,
        "void SatViewScenePass::record_prepass(IRenderContext& ctx)",
        "void SatViewScenePass::record(IRenderContext& ctx)");
    REQUIRE_FALSE(prepass.empty());
    CHECK(prepass.find("vk_ctx->frame_index() % buffered_frame_count") != std::string::npos);
    CHECK(prepass.find("auto& streams = state_->frame_streams[frame_index]") != std::string::npos);
    CHECK(prepass.find("streams.track_vertex_buffer") != std::string::npos);
    CHECK(prepass.find("streams.marker_buffer") != std::string::npos);
    CHECK(prepass.find("streams.surface_marker_buffer") != std::string::npos);
    CHECK(prepass.find("&streams.track_vertex_buffer.buffer") != std::string::npos);
    CHECK(prepass.find("&streams.marker_buffer.buffer") != std::string::npos);
    CHECK(prepass.find("&streams.surface_marker_buffer.buffer") != std::string::npos);

    // Same-capacity writes and growth can replace only the current slot's
    // resource because every upload and draw below is reached through the
    // frame-local `streams` reference above.
    const std::string upload = source_block(
        source,
        "bool ensure_vertex_buffer(const VkRenderContext& ctx",
        "bool ensure_label_texture(");
    REQUIRE_FALSE(upload.empty());
    CHECK(upload.find("buffer.size < byte_size") != std::string::npos);
    CHECK(upload.find("destroy_buffer(ctx.allocator(), buffer)") != std::string::npos);
    CHECK(upload.find("std::memcpy(buffer.mapped, items.data(), byte_size)") != std::string::npos);
    CHECK(upload.find("vmaFlushAllocation(ctx.allocator(), buffer.allocation, 0, byte_size)") != std::string::npos);
}
