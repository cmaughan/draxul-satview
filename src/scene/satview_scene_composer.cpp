#include <draxul/satview/satview_scene_composer.h>

#include <algorithm>
#include <draxul/satview/satview_ground_view.h>
#include <draxul/satview/satview_object_style.h>

namespace draxul::satview
{

namespace
{

glm::vec4 orbit_class_color(OrbitClass orbit_class, float alpha)
{
    switch (orbit_class)
    {
    case OrbitClass::LowEarth: return { 0.18f, 0.78f, 1.00f, alpha };
    case OrbitClass::MediumEarth: return { 0.46f, 0.92f, 0.42f, alpha };
    case OrbitClass::Geosynchronous: return { 1.00f, 0.72f, 0.24f, alpha };
    case OrbitClass::HighlyElliptical: return { 0.96f, 0.42f, 0.90f, alpha };
    case OrbitClass::Other: return { 0.72f, 0.78f, 0.86f, alpha };
    }
    return { 0.72f, 0.78f, 0.86f, alpha };
}

glm::vec4 object_kind_color(SatelliteObjectKind kind, float alpha)
{
    switch (kind)
    {
    case SatelliteObjectKind::Payload: return { 0.12f, 0.86f, 0.98f, alpha };
    case SatelliteObjectKind::RocketBody: return { 1.00f, 0.54f, 0.18f, alpha };
    case SatelliteObjectKind::Debris: return { 1.00f, 0.28f, 0.36f, alpha };
    case SatelliteObjectKind::Unknown: return { 0.72f, 0.78f, 0.86f, alpha };
    }
    return { 0.72f, 0.78f, 0.86f, alpha };
}

glm::vec4 population_color(SatellitePopulation population, float alpha)
{
    switch (population)
    {
    case SatellitePopulation::ActivePayload: return { 0.18f, 0.88f, 1.00f, alpha };
    case SatellitePopulation::InactivePayload: return { 0.48f, 0.55f, 0.72f, alpha };
    case SatellitePopulation::RocketBody: return { 1.00f, 0.58f, 0.18f, alpha };
    case SatellitePopulation::Debris: return { 1.00f, 0.28f, 0.36f, alpha };
    case SatellitePopulation::Unknown: return { 0.72f, 0.78f, 0.86f, alpha };
    }
    return { 0.72f, 0.78f, 0.86f, alpha };
}

glm::vec3 to_vec3(const glm::dvec3& value)
{
    return glm::vec3(value);
}

} // namespace

glm::vec4 satview_satellite_color(
    OrbitClass orbit_class,
    SatelliteObjectKind object_kind,
    SatellitePopulation population,
    std::uint32_t object_prefix_hash,
    SatViewColorMode color_mode,
    float alpha)
{
    switch (color_mode)
    {
    case SatViewColorMode::Population: return population_color(population, alpha);
    case SatViewColorMode::NamePrefix: return satellite_prefix_color(object_prefix_hash, alpha);
    case SatViewColorMode::OrbitClass: return orbit_class_color(orbit_class, alpha);
    case SatViewColorMode::ObjectType: return object_kind_color(object_kind, alpha);
    }
    return satellite_prefix_color(object_prefix_hash, alpha);
}

bool satview_marker_is_eligible(
    bool filter_visible,
    bool selected,
    bool above_horizon,
    std::size_t marker_limit,
    std::size_t& visible_eligible_count)
{
    if ((!filter_visible && !selected) || !above_horizon)
        return false;
    const bool under_limit = marker_limit == 0 || visible_eligible_count < marker_limit;
    if (filter_visible)
        ++visible_eligible_count;
    return under_limit || selected;
}

SatViewMarkerComposeResult compose_satview_markers(const SatViewMarkerComposeRequest& request)
{
    SatViewMarkerComposeResult result;
    result.generation = request.generation;
    result.markers.reserve(request.states.size());
    if (!request.filter)
        return result;

    for (std::size_t state_index = 0; state_index < request.states.size(); ++state_index)
    {
        const SatellitePropagatedState& state = request.states[state_index];
        const bool selected = request.selected_id.has_value()
            && state.norad_catalog_id == *request.selected_id;
        const bool visible = satview_filter_matches(
            *request.filter,
            make_satview_filter_candidate(state, request.source_label));
        const glm::vec3 position0 = to_vec3(
            state.teme_position_km / kSatViewEarthEquatorialRadiusKm);
        const bool above_horizon = !request.ground_horizon_occlusion
            || !request.ground_observer_render_position.has_value()
            || satview_ground_visibility_dot(
                   glm::dvec3(position0),
                   *request.ground_observer_render_position)
                > 0.0;
        if (!satview_marker_is_eligible(
                visible,
                selected,
                above_horizon,
                request.marker_limit,
                result.visible_eligible_count))
        {
            continue;
        }

        const glm::dvec3 next_position = state_index < request.next_teme_positions_km.size()
            ? request.next_teme_positions_km[state_index]
            : state.teme_position_km;
        const glm::vec3 position1 = to_vec3(
            next_position / kSatViewEarthEquatorialRadiusKm);
        const float range = glm::length(position0);
        const float base_size = request.ground_observer_render_position.has_value()
            ? satview_ground_marker_base_size(
                  glm::dvec3(position0),
                  *request.ground_observer_render_position)
                * request.ground_marker_scale
            : std::clamp(0.006f + range * 0.0022f, 0.008f, 0.026f);
        const float size = selected ? base_size * 2.2f : base_size;
        const float fidelity_alpha = state.solution_kind == OrbitSolutionKind::SatcatSummaryEstimate
            ? 0.55f
            : 1.0f;
        const glm::vec4 color = selected
            ? selected_marker_color(fidelity_alpha)
            : glm::mix(
                  satview_satellite_color(
                      state.orbit_class,
                      state.object_kind,
                      state.population,
                      state.object_prefix_hash,
                      request.color_mode,
                      0.95f * fidelity_alpha),
                  glm::vec4(1.0f, 1.0f, 1.0f, 0.95f * fidelity_alpha),
                  0.18f);
        result.markers.push_back({
            glm::vec4(position0, size),
            glm::vec4(position1, selected ? 1.0f : 0.0f),
            color,
        });
    }
    return result;
}

} // namespace draxul::satview
