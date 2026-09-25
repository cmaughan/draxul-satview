#include "satview_hdr_target_transaction.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace
{

using draxul::satview::detail::HdrTargetBuildStage;
using draxul::satview::detail::HdrTargetFailurePoint;

constexpr std::size_t stage_index(HdrTargetBuildStage stage)
{
    return static_cast<std::size_t>(stage);
}

struct FakeTarget
{
    std::array<bool, 4> resources{};
};

struct FakeTargetSet
{
    bool descriptor_pool = false;
    int generation = 0;
    std::vector<FakeTarget> targets;
};

} // namespace

TEST_CASE("SatView HDR target failures do not publish partial frames",
    "[satview][renderer][hdr]")
{
    struct FailureCase
    {
        HdrTargetFailurePoint point;
        std::size_t stages_created_before_failure;
    };
    const std::array failure_cases{
        FailureCase{ { 0, HdrTargetBuildStage::DescriptorSets }, 4 },
        FailureCase{ { 1, HdrTargetBuildStage::DebugAttachment }, 6 },
        FailureCase{ { 1, HdrTargetBuildStage::DescriptorSets }, 8 },
    };

    for (const FailureCase& failure_case : failure_cases)
    {
        CAPTURE(failure_case.point.target_index,
            stage_index(failure_case.point.stage));

        int live_resources = 0;
        FakeTargetSet published;
        published.descriptor_pool = true;
        published.generation = 1;
        published.targets.resize(1);
        ++live_resources;
        for (bool& resource : published.targets.front().resources)
        {
            resource = true;
            ++live_resources;
        }

        const auto destroy = [&](FakeTargetSet& target_set) {
            if (target_set.descriptor_pool)
            {
                target_set.descriptor_pool = false;
                --live_resources;
            }
            for (FakeTarget& target : target_set.targets)
            {
                for (bool& resource : target.resources)
                {
                    if (resource)
                    {
                        resource = false;
                        --live_resources;
                    }
                }
            }
            target_set.targets.clear();
        };
        const auto complete = [](const FakeTargetSet& target_set) {
            if (!target_set.descriptor_pool || target_set.targets.size() != 2)
                return false;
            for (const FakeTarget& target : target_set.targets)
            {
                for (const bool resource : target.resources)
                {
                    if (!resource)
                        return false;
                }
            }
            return true;
        };

        std::size_t stages_created = 0;
        const auto attempt = [&](std::optional<HdrTargetFailurePoint> failure_point) {
            return draxul::satview::detail::publish_hdr_targets_transactionally<FakeTargetSet>(
                [&](FakeTargetSet& candidate) {
                    candidate.descriptor_pool = true;
                    candidate.generation = 2;
                    candidate.targets.resize(2);
                    ++live_resources;
                    return draxul::satview::detail::build_hdr_target_frames(
                        candidate.targets.size(), failure_point,
                        [&](std::size_t target_index, HdrTargetBuildStage stage) {
                            candidate.targets[target_index].resources[stage_index(stage)] = true;
                            ++live_resources;
                            ++stages_created;
                            return true;
                        });
                },
                complete,
                destroy,
                [&](FakeTargetSet&& candidate) {
                    destroy(published);
                    published = std::move(candidate);
                });
        };

        REQUIRE_FALSE(attempt(failure_case.point));
        CHECK(stages_created == failure_case.stages_created_before_failure);
        CHECK(published.generation == 1);
        CHECK(published.targets.size() == 1);
        CHECK(live_resources == 5);

        stages_created = 0;
        REQUIRE(attempt(std::nullopt));
        CHECK(stages_created == 8);
        CHECK(published.generation == 2);
        CHECK(published.targets.size() == 2);
        CHECK(complete(published));
        CHECK(live_resources == 9);

        destroy(published);
        CHECK(live_resources == 0);
    }
}
