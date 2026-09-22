#include <catch2/catch_test_macros.hpp>
#include <draxul/satview/satview_scene_composer.h>

using namespace draxul::satview;

namespace
{

SatellitePropagatedState state_at(std::int64_t id, glm::dvec3 earth_radii)
{
    SatellitePropagatedState state;
    state.norad_catalog_id = id;
    state.population = SatellitePopulation::ActivePayload;
    state.object_kind = SatelliteObjectKind::Payload;
    state.orbit_class = OrbitClass::LowEarth;
    state.teme_position_km = earth_radii * kSatViewEarthEquatorialRadiusKm;
    return state;
}

SatelliteOrbitTrack track_at(
    std::int64_t id,
    std::initializer_list<glm::dvec3> earth_radii,
    OrbitSolutionKind solution = OrbitSolutionKind::GeneralPerturbations)
{
    SatelliteOrbitTrack track;
    track.norad_catalog_id = id;
    track.population = SatellitePopulation::ActivePayload;
    track.object_kind = SatelliteObjectKind::Payload;
    track.orbit_class = OrbitClass::LowEarth;
    track.solution_kind = solution;
    track.render_teme_points_earth_radii.assign(earth_radii);
    track.render_points_earth_radii.assign(earth_radii);
    return track;
}

} // namespace

TEST_CASE("SatView scene composer applies horizon visibility before marker limits", "[satview][scene][composer]")
{
    const std::vector states = {
        state_at(1, { -2.0, 0.0, 0.0 }),
        state_at(2, { 2.0, 0.0, 0.0 }),
        state_at(3, { 2.0, 0.1, 0.0 }),
    };
    SatViewFilterState filter;
    SatViewMarkerComposeRequest request;
    request.generation = 42;
    request.states = states;
    request.filter = &filter;
    request.marker_limit = 1;
    request.ground_observer_render_position = glm::dvec3(1.0, 0.0, 0.0);
    request.ground_horizon_occlusion = true;

    const auto result = compose_satview_markers(request);
    CHECK(result.generation == 42);
    CHECK(result.visible_eligible_count == 2);
    REQUIRE(result.markers.size() == 1);
    CHECK(result.markers.front().position0_size.x > 0.0f);
}

TEST_CASE("SatView scene composer preserves selected markers beyond filters and limits", "[satview][scene][composer]")
{
    const std::vector states = {
        state_at(1, { 2.0, 0.0, 0.0 }),
        state_at(2, { 2.0, 0.1, 0.0 }),
    };
    const std::vector next_positions = {
        glm::dvec3(2.1, 0.0, 0.0) * kSatViewEarthEquatorialRadiusKm,
        glm::dvec3(2.1, 0.1, 0.0) * kSatViewEarthEquatorialRadiusKm,
    };
    SatViewFilterState filter;
    filter.show_low_earth = false;
    SatViewMarkerComposeRequest request;
    request.states = states;
    request.next_teme_positions_km = next_positions;
    request.filter = &filter;
    request.selected_id = 2;
    request.marker_limit = 1;

    const auto result = compose_satview_markers(request);
    REQUIRE(result.markers.size() == 1);
    CHECK(result.markers.front().position1_selected.w == 1.0f);
    CHECK(result.markers.front().position1_selected.x > result.markers.front().position0_size.x);
}

TEST_CASE("SatView drawing and picking share marker eligibility ordering", "[satview][scene][composer]")
{
    std::size_t visible_count = 0;
    CHECK_FALSE(satview_marker_is_eligible(true, false, false, 1, visible_count));
    CHECK(visible_count == 0);
    CHECK(satview_marker_is_eligible(true, false, true, 1, visible_count));
    CHECK(visible_count == 1);
    CHECK_FALSE(satview_marker_is_eligible(true, false, true, 1, visible_count));
    CHECK(satview_marker_is_eligible(false, true, true, 1, visible_count));
}

TEST_CASE("SatView scene composer preserves closed and sampled track topology", "[satview][scene][composer]")
{
    const std::vector tracks = {
        track_at(1, { { 1.0, 0.0, 0.0 }, { 0.0, 2.0, 0.0 }, { -1.0, 0.0, 0.0 } }),
        track_at(2,
            { { 2.0, 0.0, 0.0 }, { 0.0, 3.0, 0.0 }, { -2.0, 0.0, 0.0 } },
            OrbitSolutionKind::SampledEphemeris),
    };
    SatViewFilterState filter;
    SatViewTrackComposeRequest request;
    request.tracks = tracks;
    request.filter = &filter;

    const auto vertices = compose_satview_tracks(request);
    // Three closed segments plus two finite-window segments, two vertices each.
    REQUIRE(vertices.size() == 10);
    CHECK(vertices.front().position.w == -1.0f);
    CHECK(vertices.back().position.w == 1.0f);
}

TEST_CASE("SatView scene composer keeps selected filtered tracks and map coordinate tags", "[satview][scene][composer]")
{
    const std::vector tracks = {
        track_at(7, { { 1.0, 0.0, 0.0 }, { 0.0, 2.0, 0.0 } }),
    };
    SatViewFilterState filter;
    filter.show_low_earth = false;
    SatViewTrackComposeRequest request;
    request.tracks = tracks;
    request.filter = &filter;
    request.selected_id = 7;
    request.projection_mode = SatViewProjectionMode::Map;
    request.camera_pov = SatViewCameraPov::Earth;

    const auto vertices = compose_satview_tracks(request);
    REQUIRE(vertices.size() == 2);
    CHECK(vertices.front().position.w == -2.0f);
    CHECK(vertices.back().position.w == 2.0f);
}

TEST_CASE("SatView visible radius honors display mode filters and next positions", "[satview][scene][composer]")
{
    const std::vector tracks = {
        track_at(1, { { 1.0, 0.0, 0.0 }, { 0.0, 3.0, 0.0 } }),
    };
    const std::vector states = { state_at(2, { 2.0, 0.0, 0.0 }) };
    const std::vector next_positions = {
        glm::dvec3(4.0, 0.0, 0.0) * kSatViewEarthEquatorialRadiusKm,
    };
    SatViewFilterState filter;

    const float all_radius = satview_visible_scene_radius({
        .tracks = tracks,
        .states = states,
        .next_teme_positions_km = next_positions,
        .filter = &filter,
    });
    CHECK(all_radius > 3.99f);
    CHECK(all_radius < 4.01f);

    const float tracks_only_radius = satview_visible_scene_radius({
        .tracks = tracks,
        .states = states,
        .next_teme_positions_km = next_positions,
        .filter = &filter,
        .satellite_display_mode = SatViewSatelliteDisplayMode::TracksOnly,
    });
    CHECK(tracks_only_radius > 2.99f);
    CHECK(tracks_only_radius < 3.01f);
}

TEST_CASE("SatView generated body tracks stay device free and paired", "[satview][scene][composer]")
{
    const auto moon = compose_satview_moon_track(1'700'000'000.0, 16);
    const auto earth = compose_satview_earth_track(1'700'000'000.0, 16);
    const auto uranus_rings = compose_satview_planetary_rings(SatViewCameraPov::Uranus);
    const auto earth_rings = compose_satview_planetary_rings(SatViewCameraPov::Earth);

    CHECK(moon.size() == 32);
    CHECK(earth.size() == 32);
    CHECK(uranus_rings.size() == 9 * 128 * 2);
    CHECK(earth_rings.empty());
    CHECK(satview_solar_system_scene_radius(SatViewCameraPov::Sun) > 1.0f);
}
