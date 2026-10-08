#include <catch2/catch_test_macros.hpp>
#include <draxul/satview/satview_ground_view.h>
#include <draxul/satview/satview_map_projection.h>
#include <draxul/satview/satview_scene_composer.h>

#include <array>
#include <cmath>
#include <numbers>

using namespace draxul::satview;

namespace
{

SatellitePropagatedState state_at(std::int64_t id, glm::dvec3 teme_earth_radii)
{
    SatellitePropagatedState state;
    state.norad_catalog_id = id;
    state.population = SatellitePopulation::ActivePayload;
    state.object_kind = SatelliteObjectKind::Payload;
    state.orbit_class = OrbitClass::LowEarth;
    state.teme_position_km = teme_earth_radii * kSatViewEarthEquatorialRadiusKm;
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
        state_at(1, { 0.0, 1.1, 0.0 }),
        state_at(2, { 0.0, -1.1, 0.0 }),
        state_at(3, { 0.0, -1.1, 0.1 }),
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
    const float expected_size = satview_ground_marker_base_size(
        glm::dvec3(1.1, 0.0, 0.0), *request.ground_observer_render_position)
        * request.ground_marker_scale;
    CHECK(std::abs(result.markers.front().position0_size.w - expected_size) < 1.0e-6f);
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
    CHECK(result.markers.front().position1_selected.z < result.markers.front().position0_size.z);
}

TEST_CASE("SatView composed markers match cardinal axes tracks and interpolated picking", "[satview][scene][composer][coordinates]")
{
    struct AxisCase
    {
        glm::dvec3 teme;
        glm::dvec3 rendered;
    };
    const std::array cases = {
        AxisCase{ { 2.0, 0.0, 0.0 }, { 0.0, 0.0, -2.0 } },
        AxisCase{ { -2.0, 0.0, 0.0 }, { 0.0, 0.0, 2.0 } },
        AxisCase{ { 0.0, 2.0, 0.0 }, { -2.0, 0.0, 0.0 } },
        AxisCase{ { 0.0, -2.0, 0.0 }, { 2.0, 0.0, 0.0 } },
        AxisCase{ { 0.0, 0.0, 2.0 }, { 0.0, 2.0, 0.0 } },
        AxisCase{ { 0.0, 0.0, -2.0 }, { 0.0, -2.0, 0.0 } },
        AxisCase{ { 2.0, 3.0, 4.0 }, { -3.0, 4.0, -2.0 } },
    };
    SatViewFilterState filter;
    for (const auto& axes : cases)
    {
        INFO("TEME " << axes.teme.x << ", " << axes.teme.y << ", " << axes.teme.z);
        const std::vector states = { state_at(1, axes.teme) };
        const std::vector next_positions = {
            (axes.teme + glm::dvec3(0.3, -0.4, 0.5)) * kSatViewEarthEquatorialRadiusKm,
        };
        const glm::dvec3 expected_next = axes.rendered + glm::dvec3(0.4, 0.5, -0.3);
        SatViewMarkerComposeRequest request;
        request.states = states;
        request.next_teme_positions_km = next_positions;
        request.filter = &filter;
        const auto result = compose_satview_markers(request);
        REQUIRE(result.markers.size() == 1);
        const auto& marker = result.markers.front();
        CHECK(glm::length(glm::dvec3(marker.position0_size) - axes.rendered) < 1.0e-6);
        CHECK(glm::length(glm::dvec3(marker.position1_selected) - expected_next) < 1.0e-6);

        const std::vector tracks = {
            track_at(1, { axes.rendered, expected_next }, OrbitSolutionKind::SampledEphemeris),
        };
        SatViewTrackComposeRequest track_request;
        track_request.tracks = tracks;
        track_request.filter = &filter;
        const auto vertices = compose_satview_tracks(track_request);
        REQUIRE(vertices.size() == 2);
        CHECK(glm::length(glm::vec3(vertices[0].position) - glm::vec3(marker.position0_size)) < 1.0e-6f);
        CHECK(glm::length(glm::vec3(vertices[1].position) - glm::vec3(marker.position1_selected)) < 1.0e-6f);

        for (const double alpha : { 0.0, 0.35, 1.0 })
        {
            const auto drawn = glm::mix(glm::dvec3(marker.position0_size), glm::dvec3(marker.position1_selected), alpha);
            const auto picked = teme_position_to_render_earth_radii(
                glm::mix(states.front().teme_position_km, next_positions.front(), alpha));
            CHECK(glm::length(drawn - picked) < 1.0e-6);
        }
        request.next_teme_positions_km = {};
        const auto stationary = compose_satview_markers(request);
        REQUIRE(stationary.markers.size() == 1);
        CHECK(glm::length(glm::vec3(stationary.markers.front().position1_selected) - glm::vec3(marker.position0_size)) < 1.0e-6f);
    }
}

TEST_CASE("SatView composed markers retain geographic map positions", "[satview][scene][composer][coordinates][map]")
{
    constexpr double timestamp = 946728000.0;
    constexpr double radians = std::numbers::pi_v<double> / 180.0;
    const double sidereal = greenwich_sidereal_angle_radians(timestamp);
    SatViewFilterState filter;
    for (const auto longitude_latitude : {
             glm::dvec2(0.0, 0.0), glm::dvec2(90.0, 0.0), glm::dvec2(-90.0, 45.0),
             glm::dvec2(120.0, -60.0), glm::dvec2(45.0, 80.0) })
    {
        const double longitude = longitude_latitude.x * radians;
        const double latitude = longitude_latitude.y * radians;
        const glm::dvec3 teme = 2.0 * glm::dvec3(
            std::cos(latitude) * std::cos(longitude + sidereal),
            std::cos(latitude) * std::sin(longitude + sidereal), std::sin(latitude));
        const std::vector states = { state_at(1, teme) };
        SatViewMarkerComposeRequest request;
        request.states = states;
        request.filter = &filter;
        const auto result = compose_satview_markers(request);
        REQUIRE(result.markers.size() == 1);
        const glm::dvec3 rendered(result.markers.front().position0_size);
        const auto geographic = satview_ground_location_from_render_position(rendered, timestamp);
        CHECK(std::abs(std::remainder(geographic.longitude_radians - longitude, 2.0 * std::numbers::pi_v<double>)) < 1.0e-6);
        CHECK(std::abs(geographic.latitude_radians - latitude) < 1.0e-6);
        const glm::dvec3 shader_teme(-rendered.z, -rendered.x, rendered.y);
        for (const auto center : { glm::vec2(0.0f), glm::vec2(0.7f, -0.2f) })
        {
            const auto drawn = satview_map_position_from_teme(shader_teme * kSatViewEarthEquatorialRadiusKm, timestamp, center);
            const auto picked = satview_map_position_from_teme(states.front().teme_position_km, timestamp, center);
            CHECK(glm::length(drawn - picked) < 1.0e-6f);
        }
    }
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
