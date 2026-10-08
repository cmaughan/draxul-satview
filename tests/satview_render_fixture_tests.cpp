// SatView render-regression fixture contracts (kanban 17).
//
// The GPU scenarios tests/render/satview-plugin.toml (globe) and
// satview-plugin-map.toml (map) capture the real plugin through its test-only
// `render_test_fixture` launch key. These CPU tests pin the two claims those
// references depend on, without a GPU:
//   1. The offline fixture payloads really place satellites over the
//      geographic poles, the equator and 45N/45E at the fixture epoch, and
//      each sampled track passes through its marker.
//   2. The runtime seam the plugin uses starts paused at the epoch, pins the
//      camera and map view, and reports ready only once the fixture catalog's
//      markers and tracks were uploaded. A scene with no markers never becomes
//      ready, so the render test (gated on content readiness) cannot pass.

#include <catch2/catch_test_macros.hpp>

#include "satview_host_fixture.h"
#include "temp_dir.h"

#include <draxul/satview/satview_catalog.h>
#include <draxul/satview/satview_config.h>
#include <draxul/satview/satview_propagation.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <string>

using namespace draxul;
using namespace draxul::satview;

namespace
{

// 2026-03-20T12:00:00Z, the EPOCH of every fixture record.
constexpr double kFixtureUnixSeconds = 1'774'008'000.0;
constexpr std::size_t kFixtureSatelliteCount = 7;

std::filesystem::path fixture_root()
{
    return std::filesystem::path(DRAXUL_SATVIEW_TEST_FIXTURE_ROOT) / "render";
}

std::string read_fixture(const char* name)
{
    std::ifstream stream(fixture_root() / name, std::ios::binary);
    return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}

struct GeographicExpectation
{
    double latitude_degrees = 0.0;
    double longitude_degrees = 0.0;
    // Near a pole a few kilometres of cross-track motion is degrees of
    // longitude; elsewhere the tolerance matches the latitude one.
    double longitude_tolerance_degrees = 0.5;
};

// The polar satellites sit 0.5 degrees (about 55 km) from each geographic
// pole rather than exactly on it, so their longitude, and therefore their
// equirectangular map position, is well defined on every platform.
const std::map<std::int64_t, GeographicExpectation>& expected_positions()
{
    static const std::map<std::int64_t, GeographicExpectation> expected{
        { 990001, { 89.5, 0.0, 5.0 } },
        { 990002, { -89.5, 90.0, 5.0 } },
        { 990003, { 0.0, 0.0 } },
        { 990004, { 0.0, 90.0 } },
        { 990005, { 0.0, 180.0 } },
        { 990006, { 0.0, -90.0 } },
        { 990007, { 45.0, 45.0 } },
    };
    return expected;
}

double longitude_delta_degrees(double a, double b)
{
    return std::abs(std::remainder(a - b, 360.0));
}

glm::dvec2 longitude_latitude_degrees(const glm::dvec3& ecef)
{
    const double radius = glm::length(ecef);
    return {
        glm::degrees(std::atan2(ecef.y, ecef.x)),
        glm::degrees(std::asin(std::clamp(ecef.z / radius, -1.0, 1.0))),
    };
}

double distance_to_polyline(const glm::dvec3& point, const std::vector<glm::dvec3>& line)
{
    double best = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i + 1 < line.size(); ++i)
    {
        const glm::dvec3 a = line[i];
        const glm::dvec3 ab = line[i + 1] - a;
        const double length_squared = glm::dot(ab, ab);
        const double t = length_squared > 0.0
            ? std::clamp(glm::dot(point - a, ab) / length_squared, 0.0, 1.0)
            : 0.0;
        best = std::min(best, glm::length(point - (a + ab * t)));
    }
    return best;
}

SatViewRenderTestFixture make_fixture(const std::filesystem::path& cache_directory)
{
    SatViewRenderTestFixture fixture;
    fixture.unix_seconds = kFixtureUnixSeconds;
    fixture.gp_json = read_fixture("satview_render_fixture_gp.json");
    fixture.satcat_csv = read_fixture("satview_render_fixture_satcat.csv");
    fixture.cache_directory = cache_directory;
    fixture.camera_longitude_degrees = 45.0;
    fixture.camera_latitude_degrees = 0.0;
    fixture.camera_distance_earth_radii = 3.6f;
    fixture.map_center_degrees = { 0.0f, 0.0f };
    fixture.required_markers = kFixtureSatelliteCount;
    fixture.required_tracks = kFixtureSatelliteCount;
    return fixture;
}

// Pumps and draws until the fixture is ready or the bound is hit.
bool draw_until_ready(OfflineSatViewHost& fixture_host, int max_iterations = 1500)
{
    return fixture_host.pump_until([&]() {
        fixture_host.draw_once();
        return fixture_host.host.render_test_fixture_ready();
    },
        max_iterations);
}

} // namespace

TEST_CASE("satview render fixture payloads place satellites at known geography",
    "[satview][render-fixture]")
{
    const auto gp = parse_celestrak_gp_json(read_fixture("satview_render_fixture_gp.json"), "active");
    const auto satcat = parse_celestrak_satcat_csv(read_fixture("satview_render_fixture_satcat.csv"));
    REQUIRE(gp);
    REQUIRE(satcat);
    REQUIRE(gp.catalog.objects.size() == kFixtureSatelliteCount);
    REQUIRE(satcat.catalog.objects.size() == kFixtureSatelliteCount);

    const SatelliteCatalog merged = merge_satellite_catalogs(gp.catalog, satcat.catalog);
    auto build = build_satellite_propagation_model(merged);
    REQUIRE(build);

    SatellitePropagationSettings settings;
    settings.track_satellite_limit = kDefaultTrackSatelliteLimit;
    settings.track_sample_count = kDefaultTrackSampleCount;
    const SatellitePropagationResult result = propagate_satellites(
        build.model, kFixtureUnixSeconds, settings);
    REQUIRE(result);
    REQUIRE(result.failed_propagations == 0);
    REQUIRE(result.states.size() == kFixtureSatelliteCount);
    REQUIRE(result.tracks.size() == kFixtureSatelliteCount);

    for (const SatellitePropagatedState& state : result.states)
    {
        INFO("NORAD " << state.norad_catalog_id);
        const auto expected = expected_positions().find(state.norad_catalog_id);
        REQUIRE(expected != expected_positions().end());
        const glm::dvec2 geographic = longitude_latitude_degrees(state.ecef_position_km);
        INFO("longitude " << geographic.x << " latitude " << geographic.y);
        CHECK(std::abs(geographic.y - expected->second.latitude_degrees) < 0.2);
        CHECK(longitude_delta_degrees(geographic.x, expected->second.longitude_degrees)
            < expected->second.longitude_tolerance_degrees);
        const double altitude_km = glm::length(state.ecef_position_km) - 6378.137;
        CHECK(altitude_km > 450.0);
        CHECK(altitude_km < 560.0);

        // The marker and its sampled track share the TEME-to-render frame:
        // the track polyline passes through the marker.
        const auto track = std::ranges::find_if(result.tracks, [&](const SatelliteOrbitTrack& candidate) {
            return candidate.norad_catalog_id == state.norad_catalog_id;
        });
        REQUIRE(track != result.tracks.end());
        const glm::dvec3 marker = teme_position_to_render_earth_radii(state.teme_position_km);
        CHECK(distance_to_polyline(marker, track->render_teme_points_earth_radii) < 0.01);
    }
}

TEST_CASE("satview render fixture seam starts paused at the epoch with a pinned view",
    "[satview][render-fixture]")
{
    tests::TempDir fixture_cache{ "satview-render-fixture" };
    OfflineSatViewHost fixture_host;
    REQUIRE(fixture_host.initialize_render_fixture(make_fixture(fixture_cache.path)));
    CHECK(fixture_host.host.render_test_fixture_active());
    CHECK_FALSE(fixture_host.host.render_test_fixture_ready());
    CHECK(fixture_host.host.paused());
    CHECK_FALSE(SatViewHostTestAccess::show_ui_panel(fixture_host.host));
    CHECK(SatViewHostTestAccess::simulated_seconds(fixture_host.host) == kFixtureUnixSeconds);

    // The camera looks at 0N 45E at the fixture epoch: undo the render and
    // sidereal rotations and recover that geographic direction.
    const glm::dvec3 render(SatViewHostTestAccess::camera_position(fixture_host.host));
    CHECK(std::abs(glm::length(render) - 3.6) < 1.0e-3);
    const glm::dvec3 teme(-render.z, -render.x, render.y);
    const double sidereal = greenwich_sidereal_angle_radians(kFixtureUnixSeconds);
    const glm::dvec3 ecef(
        std::cos(sidereal) * teme.x + std::sin(sidereal) * teme.y,
        -std::sin(sidereal) * teme.x + std::cos(sidereal) * teme.y,
        teme.z);
    const glm::dvec2 camera_geographic = longitude_latitude_degrees(ecef);
    CHECK(longitude_delta_degrees(camera_geographic.x, 45.0) < 1.0e-3);
    CHECK(std::abs(camera_geographic.y) < 1.0e-3);
    CHECK(SatViewHostTestAccess::map_center_radians(fixture_host.host) == glm::vec2(0.0f));

    REQUIRE(draw_until_ready(fixture_host));
    CHECK(SatViewHostTestAccess::uploaded_marker_count(fixture_host.host) == kFixtureSatelliteCount);
    CHECK(SatViewHostTestAccess::uploaded_track_count(fixture_host.host) == kFixtureSatelliteCount);
    // Readiness never advanced the paused clock or reached the user's cache.
    CHECK(SatViewHostTestAccess::simulated_seconds(fixture_host.host) == kFixtureUnixSeconds);
    CHECK(std::filesystem::exists(fixture_cache.path / "celestrak_active_gp.json"));
    CHECK(std::filesystem::exists(fixture_cache.path / "celestrak_satcat.csv"));

    // The map projection consumes the same fixture and stays ready.
    SatViewConfig config = SatViewHostTestAccess::current_config(fixture_host.host);
    config.projection_mode = SatViewProjectionMode::Map;
    SatViewHostTestAccess::apply_config(fixture_host.host, config);
    REQUIRE(draw_until_ready(fixture_host));
    CHECK(SatViewHostTestAccess::projection_mode(fixture_host.host) == SatViewProjectionMode::Map);
    CHECK(SatViewHostTestAccess::uploaded_marker_count(fixture_host.host) == kFixtureSatelliteCount);
}

TEST_CASE("satview render fixture never reports ready without its markers",
    "[satview][render-fixture]")
{
    SECTION("markers hidden by the display mode")
    {
        tests::TempDir fixture_cache{ "satview-render-fixture-tracks-only" };
        OfflineSatViewHost fixture_host;
        REQUIRE(fixture_host.initialize_render_fixture(make_fixture(fixture_cache.path)));
        SatViewConfig config = SatViewHostTestAccess::current_config(fixture_host.host);
        config.satellite_display_mode = SatViewSatelliteDisplayMode::TracksOnly;
        SatViewHostTestAccess::apply_config(fixture_host.host, config);

        // Wait until the fixture catalog's tracks are on screen, then keep
        // drawing: with zero markers the fixture must stay pending.
        REQUIRE(fixture_host.pump_until([&]() {
            fixture_host.draw_once();
            return SatViewHostTestAccess::catalog_is_live(fixture_host.host)
                && SatViewHostTestAccess::uploaded_track_count(fixture_host.host) == kFixtureSatelliteCount;
        },
            1500));
        for (int i = 0; i < 20; ++i)
            fixture_host.draw_once();
        CHECK(SatViewHostTestAccess::uploaded_marker_count(fixture_host.host) == 0);
        CHECK_FALSE(fixture_host.host.render_test_fixture_ready());
    }

    SECTION("fewer markers than the fixture requires")
    {
        tests::TempDir fixture_cache{ "satview-render-fixture-short" };
        OfflineSatViewHost fixture_host;
        SatViewRenderTestFixture fixture = make_fixture(fixture_cache.path);
        fixture.required_markers = kFixtureSatelliteCount + 1;
        REQUIRE(fixture_host.initialize_render_fixture(std::move(fixture)));
        REQUIRE(fixture_host.pump_until([&]() {
            fixture_host.draw_once();
            return SatViewHostTestAccess::catalog_is_live(fixture_host.host)
                && SatViewHostTestAccess::uploaded_marker_count(fixture_host.host) == kFixtureSatelliteCount
                && SatViewHostTestAccess::uploaded_track_count(fixture_host.host) == kFixtureSatelliteCount;
        },
            1500));
        for (int i = 0; i < 20; ++i)
            fixture_host.draw_once();
        CHECK_FALSE(fixture_host.host.render_test_fixture_ready());
    }
}

TEST_CASE("satview render fixture is refused once the runtime is running",
    "[satview][render-fixture]")
{
    tests::TempDir fixture_cache{ "satview-render-fixture-late" };
    OfflineSatViewHost fixture_host;
    REQUIRE(fixture_host.initialize());
    CHECK_FALSE(fixture_host.host.install_render_test_fixture(make_fixture(fixture_cache.path)));
    CHECK_FALSE(fixture_host.host.render_test_fixture_active());
    CHECK_FALSE(fixture_host.host.render_test_fixture_ready());
}
