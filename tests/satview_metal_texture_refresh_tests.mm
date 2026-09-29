#include <catch2/catch_test_macros.hpp>

#include <draxul/metal/metal_render_context.h>
#include <draxul/satview/satview_scene_pass.h>
#include <draxul/satview/satview_texture_assets.h>

#import <Metal/Metal.h>

#include <array>
#include <filesystem>
#include <memory>

using namespace draxul;
using namespace draxul::satview;

TEST_CASE("SatView Metal completes overlapping cloud and label atlas revisions",
    "[satview][metal][gpu][refresh]")
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
        SKIP("Metal device unavailable");

    const auto prior_asset_root = resolve_satview_asset_path({});
    const auto plugin_assets = std::filesystem::path(DRAXUL_SATVIEW_TEST_BUILD_ROOT)
        / "draxul.app/Contents/PlugIns/dev.draxul.satview/assets";
    REQUIRE(std::filesystem::exists(plugin_assets / "../shaders/satview_scene.metallib"));
    set_satview_asset_root(plugin_assets);
    struct AssetRootRestore
    {
        std::filesystem::path root;
        ~AssetRootRestore() { set_satview_asset_root(root); }
    } restore{ prior_asset_root };

    id<MTLCommandQueue> queue = [device newCommandQueue];
    REQUIRE(queue != nil);
    id<MTLSharedEvent> first_frame_gate = [device newSharedEvent];
    REQUIRE(first_frame_gate != nil);
    SatViewScenePass pass;
    SatViewFrameUniforms frame;
    frame.camera_pos.w = 0.0f;
    pass.set_frame(frame);
    const std::array<SatViewLabelInstance, 1> labels{ SatViewLabelInstance{} };
    pass.set_constellation_labels(labels);

    // A slot is reused only after its previous command buffer completes, as
    // MetalRenderer does. The two submitted buffers can otherwise overlap.
    std::array<id<MTLCommandBuffer>, 2> in_flight{ nil, nil };
    for (int revision = 0; revision < 8; ++revision)
    {
        const int slot = revision % 2;
        if (in_flight[slot])
        {
            [in_flight[slot] waitUntilCompleted];
            REQUIRE(in_flight[slot].status == MTLCommandBufferStatusCompleted);
        }

        auto cloud = std::make_shared<LoadedTextureImage>();
        cloud->width = 8;
        cloud->height = 8;
        cloud->rgba.resize(8 * 8 * 4, static_cast<uint8_t>(revision * 29));
        pass.set_cloud_image(cloud);

        auto atlas = std::make_shared<TextAtlasImage>();
        atlas->width = 8;
        atlas->height = 8;
        atlas->rgba.resize(8 * 8 * 4, static_cast<uint8_t>(255 - revision * 20));
        pass.set_label_atlas(atlas);

        id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
        REQUIRE(command_buffer != nil);
        MetalRenderContext context(command_buffer, nil,
            static_cast<uint32_t>(slot), 2,
            96, 96, 0, 0, 96, 96, device);
        pass.record_prepass(context);
        if (revision == 0)
            [command_buffer encodeWaitForEvent:first_frame_gate value:1];
        [command_buffer commit];
        in_flight[slot] = command_buffer;
        if (revision == 1)
        {
            // The first GPU frame is held while the second revision uploads
            // and submits. Both frame slots must retain their own readers.
            CHECK(in_flight[0].status != MTLCommandBufferStatusCompleted);
            first_frame_gate.signaledValue = 1;
        }
    }

    for (id<MTLCommandBuffer> command_buffer : in_flight)
    {
        [command_buffer waitUntilCompleted];
        CHECK(command_buffer.status == MTLCommandBufferStatusCompleted);
    }
}
