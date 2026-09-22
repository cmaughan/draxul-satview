#include <catch2/catch_test_macros.hpp>

#include "satview_view_controller.h"

#include <glm/gtc/constants.hpp>

using namespace draxul::satview;

TEST_CASE("SatView view controller changes POV and leaves ground outside Earth", "[satview][view-controller]")
{
    SatViewViewController controller;
    SatViewFilterState earth_filter;
    satview_select_central_body(earth_filter, CentralBody::Earth);

    const SatViewPovTransition moon = controller.change_pov(
        SatViewCameraPov::Earth,
        SatViewCameraPov::Moon,
        SatViewProjectionMode::Ground,
        earth_filter);
    CHECK(moon.pov == SatViewCameraPov::Moon);
    CHECK(moon.projection == SatViewProjectionMode::Globe);
    CHECK(moon.central_body == CentralBody::Moon);
    CHECK(moon.pov_changed);
    CHECK(moon.central_body_changed);

    satview_select_central_body(earth_filter, CentralBody::Moon);
    const SatViewPovTransition repeated = controller.change_pov(
        SatViewCameraPov::Moon,
        SatViewCameraPov::Moon,
        SatViewProjectionMode::Map,
        earth_filter);
    CHECK_FALSE(repeated.pov_changed);
    CHECK_FALSE(repeated.central_body_changed);
    CHECK(repeated.projection == SatViewProjectionMode::Map);
}

TEST_CASE("SatView view controller wraps map dragging", "[satview][view-controller]")
{
    SatViewViewController controller;
    const glm::vec2 center = controller.pan_map(
        glm::vec2(glm::pi<float>() - 0.1f, 0.0f),
        glm::vec2(0.2f, glm::pi<float>()));

    CHECK(center.x < -3.0f);
    CHECK(center.y <= glm::half_pi<float>());
    CHECK(center.y >= -glm::half_pi<float>());
}

TEST_CASE("SatView view controller enters Earth ground view at the requested location", "[satview][view-controller]")
{
    SatViewViewController controller;
    const SatViewGroundEntry entry = controller.enter_ground({ 1.25, -0.5 });

    CHECK(entry.pov == SatViewCameraPov::Earth);
    CHECK(entry.projection == SatViewProjectionMode::Ground);
    CHECK(entry.central_body == CentralBody::Earth);
    CHECK(entry.location_radians == glm::dvec2(1.25, -0.5));
}

TEST_CASE("SatView view controller keeps selections mutually exclusive", "[satview][view-controller]")
{
    SatViewViewController controller;

    const SatViewSelectionState satellite = controller.select_satellite(42);
    CHECK(satellite.satellite_id == 42);
    CHECK_FALSE(satellite.surface.has_value());
    CHECK_FALSE(satellite.natural_body.has_value());

    const SatViewSelectionState surface = controller.select_surface(CentralBody::Mars, 7);
    REQUIRE(surface.surface.has_value());
    CHECK(surface.surface->body == CentralBody::Mars);
    CHECK(surface.surface->catalog_index == 7);
    CHECK_FALSE(surface.satellite_id.has_value());
    CHECK_FALSE(surface.natural_body.has_value());

    const SatViewSelectionState body = controller.select_natural_body(SatViewCameraPov::Jupiter);
    CHECK(body.natural_body == SatViewCameraPov::Jupiter);
    CHECK_FALSE(body.satellite_id.has_value());
    CHECK_FALSE(body.surface.has_value());

    CHECK(controller.clear_selection().empty());
}
