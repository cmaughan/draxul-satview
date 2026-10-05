#include "temp_dir.h"
#include "test_support.h"
#include "satview_cache_publication.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <draxul/satview/satview_catalog_service.h>
#include <draxul/satview/satview_texture_assets.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using draxul::satview::SatViewCatalogService;

namespace
{

const char* kOneObjectJson = R"json([
  {
    "OBJECT_NAME": "LIVE ONE",
    "OBJECT_ID": "2026-003A",
    "EPOCH": "2026-06-26T00:00:00.000000",
    "MEAN_MOTION": 15.5,
    "ECCENTRICITY": 0.0005,
    "INCLINATION": 51.6,
    "RA_OF_ASC_NODE": 120.0,
    "ARG_OF_PERICENTER": 87.0,
    "MEAN_ANOMALY": 273.0,
    "NORAD_CAT_ID": 910001
  }
])json";

const char* kTwoObjectJson = R"json([
  {
    "OBJECT_NAME": "LIVE TWO A",
    "OBJECT_ID": "2026-004A",
    "EPOCH": "2026-06-26T00:00:00.000000",
    "MEAN_MOTION": 15.5,
    "ECCENTRICITY": 0.0005,
    "INCLINATION": 51.6,
    "RA_OF_ASC_NODE": 120.0,
    "ARG_OF_PERICENTER": 87.0,
    "MEAN_ANOMALY": 273.0,
    "NORAD_CAT_ID": 920001
  },
  {
    "OBJECT_NAME": "LIVE TWO B",
    "OBJECT_ID": "2026-004B",
    "EPOCH": "2026-06-26T00:05:00.000000",
    "MEAN_MOTION": 1.0027,
    "ECCENTRICITY": 0.0002,
    "INCLINATION": 0.1,
    "RA_OF_ASC_NODE": 5.0,
    "ARG_OF_PERICENTER": 220.0,
    "MEAN_ANOMALY": 140.0,
    "NORAD_CAT_ID": 920002
  }
])json";

const char* kOneObjectSatcat =
    "OBJECT_NAME,NORAD_CAT_ID,OBJECT_ID,OBJECT_TYPE,OPS_STATUS_CODE,OWNER,DECAY_DATE,PERIOD,"
    "INCLINATION,APOGEE,PERIGEE,RCS,DATA_STATUS_CODE,ORBIT_CENTER,ORBIT_TYPE\n"
    "LIVE ONE,910001,2026-003A,PAY,+,US,,92.9,51.6,430,410,100,,EA,ORB\n";

const char* kTwoObjectSatcat =
    "OBJECT_NAME,NORAD_CAT_ID,OBJECT_ID,OBJECT_TYPE,OPS_STATUS_CODE,OWNER,DECAY_DATE,PERIOD,"
    "INCLINATION,APOGEE,PERIGEE,RCS,DATA_STATUS_CODE,ORBIT_CENTER,ORBIT_TYPE\n"
    "LIVE TWO A,920001,2026-004A,PAY,+,US,,92.9,51.6,430,410,100,,EA,ORB\n"
    "LIVE TWO B,920002,2026-004B,PAY,+,US,,1436,0.1,35790,35770,120,,EA,ORB\n";

bool wait_for_idle(SatViewCatalogService& service)
{
    return draxul::tests::pump_until([&service] { service.pump(); },
        [&service] { return !service.refresh_in_flight(); },
        std::chrono::seconds(5), std::chrono::milliseconds(5));
}

SatViewCatalogService::Config config_for(
    const std::filesystem::path& cache_dir,
    SatViewCatalogService::FetchFunction fetch)
{
    SatViewCatalogService::Config config;
    config.cache_directory = cache_dir;
    config.refresh_interval = std::chrono::hours(2);
    config.fetch = std::move(fetch);
    return config;
}

SatViewCatalogService::FetchFunction payload_fetch(
    std::string gp,
    std::string satcat,
    int* calls = nullptr)
{
    return [gp = std::move(gp), satcat = std::move(satcat), calls](
               std::string_view url,
               std::string& error) {
        if (calls)
            ++*calls;
        error.clear();
        if (url.find("satcat.csv") != std::string_view::npos)
            return satcat;
        return gp;
    };
}

void write_file(const std::filesystem::path& path, std::string_view content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(out.good());
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("SatView cache publication preserves the destination when replacement fails",
    "[satview][catalog][service][cache]")
{
    draxul::tests::TempDir temp("satview-cache-replacement-failure");
    const auto destination = temp.path / "catalog.json";
    write_file(destination, "last valid cache");

    std::filesystem::path attempted_temporary;
    std::filesystem::path attempted_destination;
    std::string error;
    const bool published = draxul::satview::detail::write_cache_text_atomically(
        destination,
        "replacement cache",
        error,
        [&](const std::filesystem::path& temporary, const std::filesystem::path& target) {
            attempted_temporary = temporary;
            attempted_destination = target;
            return std::make_error_code(std::errc::permission_denied);
        });

    CHECK_FALSE(published);
    CHECK(attempted_destination == destination);
    CHECK(attempted_temporary.parent_path() == destination.parent_path());
    CHECK(attempted_temporary.filename().string().starts_with("catalog.json.tmp."));
    CHECK_FALSE(std::filesystem::exists(attempted_temporary));
    CHECK(read_file(destination) == "last valid cache");
    CHECK(error.find("failed to replace") != std::string::npos);
}

TEST_CASE("SatView cache publication rejects a temporary whose final close fails",
    "[satview][catalog][service][cache]")
{
    // Catalog payloads/metadata and the cloud image cache share this writer.
    const std::string destination_name = GENERATE(
        std::string("celestrak_satcat.csv"),
        std::string("celestrak_active_gp.meta"),
        std::string("live_clouds_8192x4096.jpg"));
    CAPTURE(destination_name);
    draxul::tests::TempDir temp("satview-cache-close-failure");
    const auto destination = temp.path / destination_name;
    write_file(destination, "last valid cache");

    int close_calls = 0;
    int replace_calls = 0;
    draxul::satview::detail::CacheFileOperations operations;
    operations.close_temporary = [&](std::ofstream& out) {
        // The real flush/close runs, then reports the deferred write failure
        // a full disk or network filesystem would surface only at close.
        ++close_calls;
        draxul::satview::detail::close_cache_temporary(out);
        return false;
    };
    operations.replace_file = [&](const std::filesystem::path&, const std::filesystem::path&) {
        ++replace_calls;
        return std::error_code{};
    };

    std::string error;
    const bool published = draxul::satview::detail::write_cache_text_atomically(
        destination, std::string(64 * 1024, 'x'), error, operations);

    CHECK_FALSE(published);
    CHECK(close_calls == 1);
    CHECK(replace_calls == 0);
    CHECK(read_file(destination) == "last valid cache");
    CHECK(error.find("failed to finish writing") != std::string::npos);
    std::size_t temporary_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(temp.path))
    {
        if (entry.path().filename().string().starts_with(destination_name + ".tmp."))
            ++temporary_files;
    }
    CHECK(temporary_files == 0);

    // The production close stage publishes the complete payload.
    const std::string replacement(64 * 1024 + 17, 'y');
    REQUIRE(draxul::satview::detail::write_cache_text_atomically(destination, replacement, error));
    CHECK(read_file(destination) == replacement);
}

#ifdef __APPLE__
TEST_CASE("SatView macOS cache replacement failure keeps an existing regular file",
    "[satview][catalog][service][cache][macos]")
{
    draxul::tests::TempDir temp("satview-cache-macos-replacement-failure");
    const auto destination = temp.path / "catalog.json";
    write_file(destination, "last valid cache");

    const auto original_permissions = std::filesystem::status(temp.path).permissions();
    std::error_code restrict_error;
    std::error_code restore_error;
    std::error_code native_replace_error;
    std::string error;
    const bool published = draxul::satview::detail::write_cache_text_atomically(
        destination,
        "replacement cache",
        error,
        [&](const std::filesystem::path& temporary, const std::filesystem::path& target) {
            std::filesystem::permissions(
                temp.path,
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                std::filesystem::perm_options::replace,
                restrict_error);
            if (!restrict_error)
            {
                native_replace_error = draxul::satview::detail::replace_cache_file_atomically(
                    temporary, target);
            }
            std::filesystem::permissions(
                temp.path,
                original_permissions,
                std::filesystem::perm_options::replace,
                restore_error);
            return restrict_error ? restrict_error : native_replace_error;
        });

    REQUIRE_FALSE(restrict_error);
    REQUIRE_FALSE(restore_error);
    REQUIRE(native_replace_error);
    CHECK_FALSE(published);
    CHECK(read_file(destination) == "last valid cache");
    CHECK(error.find("failed to replace") != std::string::npos);
}
#endif

#ifdef _WIN32
TEST_CASE("SatView Windows cache replacement failure keeps an existing regular file",
    "[satview][catalog][service][cache][windows]")
{
    draxul::tests::TempDir temp("satview-cache-windows-replacement-failure");
    const auto destination = temp.path / "catalog.json";
    write_file(destination, "last valid cache");

    // Denying FILE_SHARE_DELETE forces the production MoveFileExW replacement
    // to fail while the existing destination remains open and readable.
    const HANDLE locked_destination = CreateFileW(
        destination.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    REQUIRE(locked_destination != INVALID_HANDLE_VALUE);

    std::error_code native_replace_error;
    std::string error;
    const bool published = draxul::satview::detail::write_cache_text_atomically(
        destination,
        "replacement cache",
        error,
        [&](const std::filesystem::path& temporary, const std::filesystem::path& target) {
            native_replace_error = draxul::satview::detail::replace_cache_file_atomically(
                temporary, target);
            return native_replace_error;
        });
    REQUIRE(CloseHandle(locked_destination) != 0);

    INFO("MoveFileExW error: " << native_replace_error.message());
    CHECK_FALSE(published);
    CHECK((native_replace_error.value() == ERROR_SHARING_VIOLATION
        || native_replace_error.value() == ERROR_ACCESS_DENIED));
    CHECK(read_file(destination) == "last valid cache");
    CHECK(error.find("failed to replace") != std::string::npos);

    size_t temporary_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(temp.path))
    {
        if (entry.path().filename().string().starts_with("catalog.json.tmp."))
            ++temporary_files;
    }
    CHECK(temporary_files == 0);
}
#endif

TEST_CASE("SatView catalog service builds encoded CelesTrak group URLs", "[satview][catalog][service]")
{
    const auto url = SatViewCatalogService::default_celestrak_url("Visual + Active");
    CHECK(url == "https://celestrak.org/NORAD/elements/gp.php?GROUP=visual%20%2B%20active&FORMAT=json");
    CHECK(SatViewCatalogService::default_satcat_url() == "https://celestrak.org/pub/satcat.csv");
}

TEST_CASE("SatView catalog service fetches live catalog and writes cache", "[satview][catalog][service]")
{
    draxul::tests::TempDir temp("satview-catalog-service");
    int fetch_calls = 0;
    SatViewCatalogService service;

    service.start(config_for(temp.path, [&](std::string_view url, std::string& error) {
        ++fetch_calls;
        error.clear();
        if (url.find("satcat.csv") != std::string_view::npos)
            return std::string(kOneObjectSatcat);
        CHECK(url.find("GROUP=active") != std::string_view::npos);
        return std::string(kOneObjectJson);
    }));

    REQUIRE(wait_for_idle(service));
    const auto status = service.status();
    CHECK(fetch_calls == 2);
    CHECK(status.data_source == SatViewCatalogService::DataSource::Live);
    CHECK(status.refresh_state == SatViewCatalogService::RefreshState::Idle);
    CHECK(status.gp.data_source == SatViewCatalogService::DataSource::Live);
    CHECK(status.satcat.data_source == SatViewCatalogService::DataSource::Live);
    CHECK(status.object_count == 1);
    CHECK(status.renderable_count == 1);
    CHECK(status.text.find("GP: live 1") != std::string::npos);
    CHECK(status.text.find("SATCAT: live 1") != std::string::npos);
    CHECK(std::filesystem::exists(temp.path / "celestrak_active_gp.json"));
    CHECK(std::filesystem::exists(temp.path / "celestrak_active_gp.meta"));
    CHECK(std::filesystem::exists(temp.path / "celestrak_satcat.csv"));
    CHECK(std::filesystem::exists(temp.path / "celestrak_satcat.meta"));
}

TEST_CASE("SatView catalog service uses fresh cache without refetching", "[satview][catalog][service]")
{
    draxul::tests::TempDir temp("satview-catalog-service");
    {
        SatViewCatalogService seeder;
        seeder.start(config_for(temp.path, payload_fetch(kOneObjectJson, kOneObjectSatcat)));
        REQUIRE(wait_for_idle(seeder));
    }

    int fetch_calls = 0;
    SatViewCatalogService service;
    service.start(config_for(temp.path, [&](std::string_view, std::string& error) {
        ++fetch_calls;
        error = "should not fetch";
        return std::string{};
    }));

    service.pump();
    const auto status = service.status();
    CHECK(fetch_calls == 0);
    CHECK(status.data_source == SatViewCatalogService::DataSource::Cache);
    CHECK(status.refresh_state == SatViewCatalogService::RefreshState::Idle);
    CHECK(status.gp.data_source == SatViewCatalogService::DataSource::Cache);
    CHECK(status.satcat.data_source == SatViewCatalogService::DataSource::Cache);
    CHECK(status.object_count == 1);
    CHECK(status.text.find("GP: cached 1") != std::string::npos);
    CHECK(status.text.find("SATCAT: cached 1") != std::string::npos);
}

TEST_CASE("SatView catalog service skips manual refresh while cache is fresh", "[satview][catalog][service]")
{
    draxul::tests::TempDir temp("satview-catalog-service");
    {
        SatViewCatalogService seeder;
        seeder.start(config_for(temp.path, payload_fetch(kOneObjectJson, kOneObjectSatcat)));
        REQUIRE(wait_for_idle(seeder));
    }

    int fetch_calls = 0;
    SatViewCatalogService service;
    service.start(config_for(temp.path, [&](std::string_view, std::string& error) {
        ++fetch_calls;
        error = "should not fetch";
        return std::string{};
    }));

    CHECK_FALSE(service.request_refresh());
    service.pump();
    const auto status = service.status();
    CHECK(fetch_calls == 0);
    CHECK(status.data_source == SatViewCatalogService::DataSource::Cache);
    CHECK(status.refresh_state == SatViewCatalogService::RefreshState::Idle);
    CHECK(status.object_count == 1);
    CHECK(status.text.find("GP: cached 1") != std::string::npos);
}

TEST_CASE("SatView catalog service refreshes stale cache", "[satview][catalog][service]")
{
    draxul::tests::TempDir temp("satview-catalog-service");
    {
        SatViewCatalogService seeder;
        seeder.start(config_for(temp.path, payload_fetch(kOneObjectJson, kOneObjectSatcat)));
        REQUIRE(wait_for_idle(seeder));
    }

    int fetch_calls = 0;
    auto config = config_for(temp.path, payload_fetch(kTwoObjectJson, kTwoObjectSatcat, &fetch_calls));
    config.refresh_interval = std::chrono::seconds::zero();
    config.satcat_refresh_interval = std::chrono::seconds::zero();

    SatViewCatalogService service;
    service.start(std::move(config));
    REQUIRE(wait_for_idle(service));

    const auto status = service.status();
    CHECK(fetch_calls == 2);
    CHECK(status.data_source == SatViewCatalogService::DataSource::Live);
    CHECK(status.object_count == 2);
    CHECK(status.text.find("merged 2") != std::string::npos);
}

TEST_CASE("SatView catalog service keeps stale cache when refresh fails", "[satview][catalog][service]")
{
    draxul::tests::TempDir temp("satview-catalog-service");
    {
        SatViewCatalogService seeder;
        seeder.start(config_for(temp.path, payload_fetch(kOneObjectJson, kOneObjectSatcat)));
        REQUIRE(wait_for_idle(seeder));
    }

    auto config = config_for(temp.path, [](std::string_view, std::string& error) {
        error = "network unavailable";
        return std::string{};
    });
    config.refresh_interval = std::chrono::seconds::zero();
    config.satcat_refresh_interval = std::chrono::seconds::zero();

    SatViewCatalogService service;
    service.start(std::move(config));
    REQUIRE(wait_for_idle(service));

    const auto status = service.status();
    CHECK(status.data_source == SatViewCatalogService::DataSource::Cache);
    CHECK(status.refresh_state == SatViewCatalogService::RefreshState::Failed);
    CHECK(status.object_count == 1);
    CHECK(status.error.find("GP: network unavailable") != std::string::npos);
    CHECK(status.error.find("SATCAT: network unavailable") != std::string::npos);
    CHECK(status.text.find("GP: cached 1") != std::string::npos);
    CHECK(status.text.find("SATCAT: cached 1") != std::string::npos);
}

TEST_CASE("SatView catalog service rejects excessively nested GP JSON at every entry point",
    "[satview][catalog][service][json]")
{
    const std::size_t depth = 200'000;
    const std::string deep_gp = R"([{"NORAD_CAT_ID":1,"EXTRA":)" + std::string(depth, '[')
        + std::string(depth, ']') + "}]";
    draxul::tests::TempDir temp("satview-catalog-service-deep-json");
    {
        SatViewCatalogService seeder;
        seeder.start(config_for(temp.path, payload_fetch(kOneObjectJson, kOneObjectSatcat)));
        REQUIRE(wait_for_idle(seeder));
    }
    const auto gp_cache = temp.path / "celestrak_active_gp.json";

    SECTION("a network refresh keeps the usable catalog and the cached payload")
    {
        auto config = config_for(temp.path, payload_fetch(deep_gp, kOneObjectSatcat));
        config.refresh_interval = std::chrono::seconds::zero();
        SatViewCatalogService service;
        service.start(std::move(config));
        REQUIRE(wait_for_idle(service));

        const auto status = service.status();
        CHECK(status.gp.data_source == SatViewCatalogService::DataSource::Cache);
        CHECK(status.object_count == 1);
        CHECK(status.error.find("JSON nesting exceeds") != std::string::npos);
        CHECK(read_file(gp_cache) == kOneObjectJson);
    }
    SECTION("a nested startup cache falls back to the bundled sample")
    {
        write_file(gp_cache, deep_gp);
        std::filesystem::remove(temp.path / "celestrak_satcat.csv");
        std::filesystem::remove(temp.path / "celestrak_satcat.meta");
        SatViewCatalogService service;
        service.start(config_for(temp.path, [](std::string_view, std::string& error) {
            error = "offline";
            return std::string{};
        }));
        REQUIRE(wait_for_idle(service));

        const auto status = service.status();
        CHECK(status.data_source == SatViewCatalogService::DataSource::Sample);
        CHECK(status.object_count == 4);
        CHECK(status.gp.data_source == SatViewCatalogService::DataSource::None);
    }
}

TEST_CASE("SatView catalog service refreshes GP and SATCAT on independent cadences", "[satview][catalog][service]")
{
    draxul::tests::TempDir temp("satview-catalog-service");
    {
        SatViewCatalogService seeder;
        seeder.start(config_for(temp.path, payload_fetch(kOneObjectJson, kOneObjectSatcat)));
        REQUIRE(wait_for_idle(seeder));
    }

    int fetch_calls = 0;
    auto config = config_for(temp.path, [&](std::string_view url, std::string& error) {
        ++fetch_calls;
        CHECK(url.find("GROUP=active") != std::string_view::npos);
        error.clear();
        return std::string(kTwoObjectJson);
    });
    config.refresh_interval = std::chrono::seconds::zero();
    config.satcat_refresh_interval = std::chrono::hours(12);

    SatViewCatalogService service;
    service.start(std::move(config));
    REQUIRE(wait_for_idle(service));

    const auto status = service.status();
    CHECK(fetch_calls == 1);
    CHECK(status.gp.data_source == SatViewCatalogService::DataSource::Live);
    CHECK(status.satcat.data_source == SatViewCatalogService::DataSource::Cache);
    CHECK(status.object_count == 3);
}

TEST_CASE("SatView catalog service keeps one cached source when the other refresh fails", "[satview][catalog][service]")
{
    draxul::tests::TempDir temp("satview-catalog-service");
    {
        SatViewCatalogService seeder;
        seeder.start(config_for(temp.path, payload_fetch(kOneObjectJson, kOneObjectSatcat)));
        REQUIRE(wait_for_idle(seeder));
    }

    auto config = config_for(temp.path, [&](std::string_view url, std::string& error) {
        if (url.find("satcat.csv") != std::string_view::npos)
        {
            error = "SATCAT unavailable";
            return std::string{};
        }
        error.clear();
        return std::string(kTwoObjectJson);
    });
    config.refresh_interval = std::chrono::seconds::zero();
    config.satcat_refresh_interval = std::chrono::seconds::zero();

    SatViewCatalogService service;
    service.start(std::move(config));
    REQUIRE(wait_for_idle(service));

    const auto status = service.status();
    CHECK(status.refresh_state == SatViewCatalogService::RefreshState::Failed);
    CHECK(status.gp.data_source == SatViewCatalogService::DataSource::Live);
    CHECK(status.satcat.data_source == SatViewCatalogService::DataSource::Cache);
    CHECK(status.satcat.error == "SATCAT unavailable");
    CHECK(status.object_count == 3);
}

TEST_CASE("SatView catalog service retires a completed worker before refreshing again", "[satview][catalog][service][thread]")
{
    draxul::tests::TempDir temp("satview-catalog-service-retire");
    std::atomic<int> calls{ 0 };
    std::atomic<bool> second_fetch_completed{ false };
    auto config = config_for(temp.path, [&](std::string_view url, std::string& error) {
        const int call = calls.fetch_add(1) + 1;
        error.clear();
        if (call == 2)
            second_fetch_completed.store(true, std::memory_order_release);
        return url.find("satcat.csv") != std::string_view::npos
            ? std::string(kOneObjectSatcat)
            : std::string(kOneObjectJson);
    });
    config.refresh_interval = std::chrono::seconds::zero();
    config.satcat_refresh_interval = std::chrono::seconds::zero();

    SatViewCatalogService service;
    service.start(std::move(config));
    REQUIRE(draxul::tests::pump_until(
        [] {},
        [&] { return second_fetch_completed.load(std::memory_order_acquire); },
        std::chrono::seconds(5), std::chrono::milliseconds(1)));

    // The worker has returned but remains joinable until request_refresh pumps
    // its result. Replacing it here used to call std::terminate.
    bool restarted = false;
    REQUIRE(draxul::tests::pump_until(
        [&] { restarted = service.request_refresh(); },
        [&] { return restarted; },
        std::chrono::seconds(5), std::chrono::milliseconds(1)));
    REQUIRE(wait_for_idle(service));
    CHECK(calls.load() >= 4);
}

TEST_CASE("SatView concurrent catalog publishers leave a coherent reusable cache", "[satview][catalog][service][cache]")
{
    draxul::tests::TempDir temp("satview-catalog-service-concurrent");
    std::atomic<int> ready{ 0 };
    const auto concurrent_fetch = [&](std::string_view url, std::string& error) {
        ready.fetch_add(1, std::memory_order_release);
        while (ready.load(std::memory_order_acquire) < 2)
            std::this_thread::yield();
        error.clear();
        return url.find("satcat.csv") != std::string_view::npos
            ? std::string(kOneObjectSatcat)
            : std::string(kOneObjectJson);
    };

    SatViewCatalogService first;
    SatViewCatalogService second;
    first.start(config_for(temp.path, concurrent_fetch));
    second.start(config_for(temp.path, concurrent_fetch));
    REQUIRE(wait_for_idle(first));
    REQUIRE(wait_for_idle(second));

    int offline_fetches = 0;
    SatViewCatalogService offline;
    offline.start(config_for(temp.path, [&](std::string_view, std::string& error) {
        ++offline_fetches;
        error = "offline";
        return std::string{};
    }));
    offline.pump();
    CHECK(offline_fetches == 0);
    CHECK(offline.status().data_source == SatViewCatalogService::DataSource::Cache);
    CHECK(offline.status().object_count == 1);
}

TEST_CASE("SatView catalog workers keep their own asset root while another pane resets the default",
    "[satview][catalog][service][assets][thread]")
{
    // Restores the shared default even if an assertion fails part-way through.
    struct RestoreDefaultAssetRoot
    {
        ~RestoreDefaultAssetRoot()
        {
            draxul::satview::set_satview_asset_root(
                std::filesystem::path(DRAXUL_SATVIEW_TEST_ASSET_ROOT));
        }
    } restore;

    draxul::tests::TempDir temp("satview-catalog-asset-root");
    const auto service_assets = temp.path / "service-assets";
    std::filesystem::create_directories(service_assets / "catalog");
    std::string sample_json = kOneObjectJson;
    sample_json.replace(sample_json.find("910001"), 6, "940001");
    write_file(service_assets / "catalog/sample_gp.json", sample_json);
    write_file(service_assets / "catalog/lunar_dispositions.csv",
        "NORAD_CAT_ID,DISPOSITION\n930002,SURFACE\n");
    const auto cache_dir = temp.path / "cache";
    std::filesystem::create_directories(cache_dir);

    // The default root starts out (and is later reset) somewhere with no
    // assets, so any worker read through the shared default is observable.
    const auto missing_assets = temp.path / "missing-assets";
    draxul::satview::set_satview_asset_root(missing_assets);

    std::atomic<bool> worker_fetching{ false };
    std::atomic<bool> release_worker{ false };
    auto config = config_for(cache_dir, [&](std::string_view url, std::string& error) {
        error.clear();
        if (url.find("satcat.csv") != std::string_view::npos)
        {
            return std::string(
                "OBJECT_NAME,NORAD_CAT_ID,OBJECT_TYPE,DECAY_DATE,ORBIT_CENTER,ORBIT_TYPE\n"
                "LUNAR KEEP,930001,PAY,,MO,ORB\n"
                "LUNAR LANDED,930002,PAY,,MO,ORB\n");
        }
        worker_fetching.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release_worker.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return std::string(kOneObjectJson);
    });
    config.asset_root = service_assets;

    SatViewCatalogService service;
    service.start(std::move(config));

    // Startup fallback resolved the offline sample from the service root.
    const auto contains = [](const draxul::satview::SatelliteCatalog& catalog, std::int64_t id) {
        return std::ranges::any_of(catalog.objects,
            [id](const auto& record) { return record.norad_catalog_id == id; });
    };
    CHECK(contains(service.catalog(), 940001));

    // Another pane opening rewrites the shared default while the worker runs.
    REQUIRE(draxul::tests::pump_until([] {},
        [&] { return worker_fetching.load(std::memory_order_acquire); },
        std::chrono::seconds(5), std::chrono::milliseconds(1)));
    std::atomic<bool> stop_mutator{ false };
    std::thread pane_creator([&] {
        int i = 0;
        while (!stop_mutator.load(std::memory_order_acquire))
            draxul::satview::set_satview_asset_root(
                missing_assets / std::to_string(i++ % 4));
    });
    release_worker.store(true, std::memory_order_release);
    const bool idle = wait_for_idle(service);
    stop_mutator.store(true, std::memory_order_release);
    pane_creator.join();
    REQUIRE(idle);

    const auto catalog = service.catalog();
    CHECK(contains(catalog, 910001));
    CHECK(contains(catalog, 930001));
    CHECK_FALSE(contains(catalog, 930002));
    CHECK(catalog.excluded_records >= 1);
}
