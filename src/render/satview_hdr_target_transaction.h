#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace draxul::satview::detail
{

enum class HdrTargetBuildStage
{
    SceneTargets,
    DebugAttachment,
    DebugFramebuffer,
    DescriptorSets,
};

struct HdrTargetFailurePoint
{
    std::size_t target_index = 0;
    HdrTargetBuildStage stage = HdrTargetBuildStage::SceneTargets;
};

// The optional failure point is applied after the selected creation callback.
// Tests can therefore model an API that has populated a handle before reporting
// failure and verify that the enclosing transaction still releases it.
template <typename CreateStage>
bool build_hdr_target_frames(
    std::size_t frame_count,
    const std::optional<HdrTargetFailurePoint>& failure_point,
    CreateStage&& create_stage)
{
    constexpr std::array stages{
        HdrTargetBuildStage::SceneTargets,
        HdrTargetBuildStage::DebugAttachment,
        HdrTargetBuildStage::DebugFramebuffer,
        HdrTargetBuildStage::DescriptorSets,
    };

    for (std::size_t target_index = 0; target_index < frame_count; ++target_index)
    {
        for (const HdrTargetBuildStage stage : stages)
        {
            if (!create_stage(target_index, stage))
                return false;
            if (failure_point.has_value()
                && failure_point->target_index == target_index
                && failure_point->stage == stage)
                return false;
        }
    }
    return true;
}

template <typename Candidate, typename Build, typename Complete, typename Destroy, typename Publish>
bool publish_hdr_targets_transactionally(
    Build&& build,
    Complete&& complete,
    Destroy&& destroy,
    Publish&& publish)
{
    Candidate candidate{};
    if (!build(candidate) || !complete(candidate))
    {
        destroy(candidate);
        return false;
    }
    publish(std::move(candidate));
    return true;
}

} // namespace draxul::satview::detail
