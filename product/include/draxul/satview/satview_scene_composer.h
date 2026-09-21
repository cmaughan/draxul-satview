#pragma once

#include <cstdint>
#include <draxul/satview/satview_config.h>
#include <draxul/satview/satview_filter.h>
#include <draxul/satview/satview_propagation.h>
#include <draxul/satview/satview_scene_pass.h>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace draxul::satview
{

struct SatViewMarkerComposeRequest
{
    std::uint64_t generation = 0;
    std::span<const SatellitePropagatedState> states;
    std::span<const glm::dvec3> next_teme_positions_km;
    const SatViewFilterState* filter = nullptr;
    std::string_view source_label;
    std::optional<std::int64_t> selected_id;
    SatViewColorMode color_mode = SatViewColorMode::Population;
    std::size_t marker_limit = 0;
    std::optional<glm::dvec3> ground_observer_render_position;
    bool ground_horizon_occlusion = false;
    float ground_marker_scale = 0.1f;
};

struct SatViewMarkerComposeResult
{
    std::uint64_t generation = 0;
    std::size_t visible_eligible_count = 0;
    std::vector<SatViewMarkerInstance> markers;
};

[[nodiscard]] glm::vec4 satview_satellite_color(
    OrbitClass orbit_class,
    SatelliteObjectKind object_kind,
    SatellitePopulation population,
    std::uint32_t object_prefix_hash,
    SatViewColorMode color_mode,
    float alpha);

// Eligibility is shared by drawing and picking: filtered selected objects are
// retained, horizon-occluded objects do not consume the cap, and selected
// objects remain visible beyond the cap.
[[nodiscard]] bool satview_marker_is_eligible(
    bool filter_visible,
    bool selected,
    bool above_horizon,
    std::size_t marker_limit,
    std::size_t& visible_eligible_count);

[[nodiscard]] SatViewMarkerComposeResult compose_satview_markers(
    const SatViewMarkerComposeRequest& request);

} // namespace draxul::satview
