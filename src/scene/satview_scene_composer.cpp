#include <draxul/satview/satview_scene_composer.h>

#include <algorithm>
#include <draxul/satview/satview_ground_view.h>
#include <draxul/satview/satview_moon_ephemeris.h>
#include <draxul/satview/satview_object_style.h>
#include <draxul/satview/satview_solar_system.h>
#include <draxul/satview/satview_sun_ephemeris.h>
#include <numbers>

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

void append_line(
    std::vector<SatViewSceneVertex>& vertices,
    glm::vec3 a,
    glm::vec3 b,
    glm::vec4 color,
    bool earth_fixed = false)
{
    const float coordinate_tag = earth_fixed ? 2.0f : 1.0f;
    vertices.push_back({ glm::vec4(a, -coordinate_tag), color, glm::vec4(b, coordinate_tag) });
    vertices.push_back({ glm::vec4(b, coordinate_tag), color, glm::vec4(a, coordinate_tag) });
}

void append_ground_track_arc(
    std::vector<SatViewSceneVertex>& vertices,
    const glm::dvec3& start,
    const glm::dvec3& end,
    glm::vec4 color,
    const glm::dvec3& observer_render_position,
    bool horizon_occlusion)
{
    const std::size_t subdivisions = satview_ground_track_subdivision_count(
        start,
        end,
        observer_render_position);
    glm::dvec3 previous = start;
    for (std::size_t step = 1; step <= subdivisions; ++step)
    {
        const glm::dvec3 current = satview_interpolate_track_arc(
            start,
            end,
            static_cast<double>(step) / static_cast<double>(subdivisions));
        if (!horizon_occlusion
            || (satview_ground_visibility_dot(previous, observer_render_position) > 0.0
                && satview_ground_visibility_dot(current, observer_render_position) > 0.0))
        {
            append_line(vertices, to_vec3(previous), to_vec3(current), color);
        }
        previous = current;
    }
}

bool track_visible(
    const SatViewFilterState& filter,
    const SatelliteOrbitTrack& track,
    std::string_view source_label)
{
    return satview_filter_matches(filter, make_satview_filter_candidate(track, source_label));
}

bool state_visible(
    const SatViewFilterState& filter,
    const SatellitePropagatedState& state,
    std::string_view source_label)
{
    return satview_filter_matches(filter, make_satview_filter_candidate(state, source_label));
}

std::vector<SatViewSceneVertex> compose_closed_track(
    std::span<const glm::dvec3> points,
    glm::vec4 color)
{
    std::vector<SatViewSceneVertex> vertices;
    if (points.size() < 2)
        return vertices;
    vertices.reserve(points.size() * 2);
    for (std::size_t index = 1; index < points.size(); ++index)
        append_line(vertices, to_vec3(points[index - 1]), to_vec3(points[index]), color);
    append_line(vertices, to_vec3(points.back()), to_vec3(points.front()), color);
    return vertices;
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

std::vector<SatViewSceneVertex> compose_satview_tracks(
    const SatViewTrackComposeRequest& request)
{
    std::vector<SatViewSceneVertex> vertices;
    if (!request.filter)
        return vertices;

    for (const SatelliteOrbitTrack& track : request.tracks)
    {
        const bool selected = request.selected_id.has_value()
            && track.norad_catalog_id == *request.selected_id;
        if (request.display_mode == SatViewTrackDisplayMode::SelectedOnly && !selected)
            continue;
        if (!selected && !track_visible(*request.filter, track, request.source_label))
            continue;

        const float fidelity_alpha = track.solution_kind == OrbitSolutionKind::SatcatSummaryEstimate
            ? 0.55f
            : 1.0f;
        const glm::vec4 color = selected
            ? glm::mix(
                  satview_satellite_color(track.orbit_class, track.object_kind, track.population,
                      track.object_prefix_hash, request.color_mode, 0.98f * fidelity_alpha),
                  glm::vec4(1.0f, 1.0f, 1.0f, 0.98f * fidelity_alpha),
                  0.38f)
            : satview_satellite_color(track.orbit_class, track.object_kind, track.population,
                  track.object_prefix_hash, request.color_mode, 0.62f * fidelity_alpha);
        const bool earth_ground_track = request.projection_mode == SatViewProjectionMode::Map
            && request.camera_pov == SatViewCameraPov::Earth;
        const auto& points = earth_ground_track
            ? track.render_points_earth_radii
            : track.render_teme_points_earth_radii;
        if (points.size() < 2)
            continue;

        const glm::dvec3 track_render_offset = earth_ground_track
            ? glm::dvec3(0.0)
            : teme_position_to_render_earth_radii(
                  satellite_track_anchor_offset_km(track, request.simulation_seconds));
        for (std::size_t index = 1; index < points.size(); ++index)
        {
            const glm::dvec3 start = points[index - 1] + track_render_offset;
            const glm::dvec3 end = points[index] + track_render_offset;
            if (request.ground_horizon_occlusion
                && request.ground_observer_render_position.has_value()
                && (satview_ground_visibility_dot(
                        start, *request.ground_observer_render_position)
                        <= 0.0
                    || satview_ground_visibility_dot(
                           end, *request.ground_observer_render_position)
                        <= 0.0))
            {
                continue;
            }
            if (request.ground_observer_render_position.has_value())
            {
                append_ground_track_arc(
                    vertices,
                    start,
                    end,
                    color,
                    *request.ground_observer_render_position,
                    request.ground_horizon_occlusion);
            }
            else
            {
                append_line(
                    vertices,
                    to_vec3(start),
                    to_vec3(end),
                    color,
                    earth_ground_track);
            }
        }

        const glm::dvec3 closing_start = points.back() + track_render_offset;
        const glm::dvec3 closing_end = points.front() + track_render_offset;
        const bool closing_segment_visible = !request.ground_horizon_occlusion
            || !request.ground_observer_render_position.has_value()
            || (satview_ground_visibility_dot(
                    closing_start, *request.ground_observer_render_position)
                    > 0.0
                && satview_ground_visibility_dot(
                       closing_end, *request.ground_observer_render_position)
                    > 0.0);
        const bool closed_orbit = track.solution_kind != OrbitSolutionKind::SampledEphemeris;
        if (closed_orbit && !earth_ground_track && closing_segment_visible)
        {
            const glm::vec4 closing_color = color * glm::vec4(1.0f, 1.0f, 1.0f, 0.82f);
            if (request.ground_observer_render_position.has_value())
            {
                append_ground_track_arc(
                    vertices,
                    closing_start,
                    closing_end,
                    closing_color,
                    *request.ground_observer_render_position,
                    request.ground_horizon_occlusion);
            }
            else
            {
                append_line(
                    vertices,
                    to_vec3(closing_start),
                    to_vec3(closing_end),
                    closing_color);
            }
        }
    }
    return vertices;
}

std::vector<SatViewSceneVertex> compose_satview_moon_track(
    double center_seconds,
    std::size_t segment_count)
{
    const auto points = satview_moon_orbit_track(center_seconds, segment_count);
    return compose_closed_track(points, glm::vec4(0.92f, 0.86f, 0.66f, 0.82f));
}

std::vector<SatViewSceneVertex> compose_satview_earth_track(
    double center_seconds,
    std::size_t segment_count)
{
    const auto points = satview_earth_orbit_track(center_seconds, segment_count);
    return compose_closed_track(points, glm::vec4(0.30f, 0.68f, 1.00f, 0.88f));
}

std::vector<SatViewSceneVertex> compose_satview_natural_body_tracks(
    SatViewCameraPov parent_id,
    const SatViewPlanetTrackConfig& planet_tracks,
    std::size_t segment_count)
{
    std::vector<SatViewSceneVertex> vertices;
    for (const SatViewBodyOrbitTrack& track :
        satview_child_orbit_tracks(parent_id, planet_tracks, segment_count))
    {
        if (track.points_focus_radii.size() < 2)
            continue;
        for (std::size_t index = 1; index < track.points_focus_radii.size(); ++index)
        {
            append_line(vertices,
                to_vec3(track.points_focus_radii[index - 1]),
                to_vec3(track.points_focus_radii[index]),
                track.color);
        }
        append_line(vertices,
            to_vec3(track.points_focus_radii.back()),
            to_vec3(track.points_focus_radii.front()),
            track.color * glm::vec4(1.0f, 1.0f, 1.0f, 0.82f));
    }
    return vertices;
}

std::vector<SatViewSceneVertex> compose_satview_planetary_rings(SatViewCameraPov body_id)
{
    std::vector<SatViewSceneVertex> vertices;
    if (body_id != SatViewCameraPov::Uranus)
        return vertices;

    constexpr std::size_t ring_count = 9;
    constexpr double inner_radius = 1.57;
    constexpr double outer_radius = 2.00;
    constexpr std::size_t segments = 128;
    vertices.reserve(ring_count * segments * 2);
    for (std::size_t ring = 0; ring < ring_count; ++ring)
    {
        const double t = static_cast<double>(ring) / static_cast<double>(ring_count - 1);
        const double radius = glm::mix(inner_radius, outer_radius, t);
        const glm::vec4 color(0.52f, 0.72f, 0.75f, 0.34f);
        glm::vec3 previous(static_cast<float>(radius), 0.0f, 0.0f);
        for (std::size_t segment = 1; segment <= segments; ++segment)
        {
            const double angle = 2.0 * std::numbers::pi
                * static_cast<double>(segment) / static_cast<double>(segments);
            const glm::vec3 current(
                static_cast<float>(radius * std::cos(angle)),
                0.0f,
                static_cast<float>(radius * std::sin(angle)));
            append_line(vertices, previous, current, color);
            previous = current;
        }
    }
    return vertices;
}

float satview_visible_scene_radius(const SatViewVisibleRadiusRequest& request)
{
    if (!request.filter)
        return 1.0f;

    float radius = 1.0f;
    if (request.satellite_display_mode != SatViewSatelliteDisplayMode::MarkersOnly)
    {
        for (const SatelliteOrbitTrack& track : request.tracks)
        {
            const bool selected = request.selected_id.has_value()
                && track.norad_catalog_id == *request.selected_id;
            if (request.track_display_mode == SatViewTrackDisplayMode::SelectedOnly && !selected)
                continue;
            if (!track_visible(*request.filter, track, request.source_label))
                continue;
            for (const glm::dvec3& point : track.render_teme_points_earth_radii)
                radius = std::max(radius, glm::length(to_vec3(point)));
        }
    }

    if (request.satellite_display_mode == SatViewSatelliteDisplayMode::TracksOnly)
        return radius;

    for (std::size_t index = 0; index < request.states.size(); ++index)
    {
        const SatellitePropagatedState& state = request.states[index];
        if (!state_visible(*request.filter, state, request.source_label))
            continue;
        radius = std::max(radius,
            glm::length(to_vec3(
                state.teme_position_km / kSatViewEarthEquatorialRadiusKm)));
        if (index < request.next_teme_positions_km.size())
        {
            radius = std::max(radius,
                glm::length(to_vec3(
                    request.next_teme_positions_km[index]
                    / kSatViewEarthEquatorialRadiusKm)));
        }
    }
    return radius;
}

float satview_solar_system_scene_radius(SatViewCameraPov pov)
{
    const SatViewSolarSystemBody& parent = satview_solar_system_body(pov);
    float radius = 1.0f;
    for (const SatViewSolarSystemBody* child : satview_child_bodies(pov))
    {
        const double apoapsis_km = child->semi_major_axis_km * (1.0 + child->eccentricity);
        const double child_radius = child->equatorial_radius_km / parent.equatorial_radius_km;
        radius = std::max(radius,
            static_cast<float>(apoapsis_km / parent.equatorial_radius_km + child_radius));
    }
    return radius;
}

} // namespace draxul::satview
