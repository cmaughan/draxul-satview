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
