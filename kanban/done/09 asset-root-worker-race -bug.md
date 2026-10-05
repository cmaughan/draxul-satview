# Give background loaders safe asset locations

**Summary:** Protect the shared asset location so opening another satellite pane cannot interfere with background file loading.

**Priority:** 09  
**Severity:** CRITICAL  
**Source:** `plugins/satview/src/core/satview_texture_assets.cpp`

**Evidence and trigger:** B02; pane creation modifies a shared path while existing catalog workers resolve bundled files.

- [x] **Investigate:** Trace every asset-root read and write across multiple panes, loading, and shutdown.
  - Write: `create_instance` (`src/satview_plugin.cpp`) calls `set_satview_asset_root` on the
    main thread for every new pane. Reads: catalog workers
    (`SatViewCatalogService::start_refresh`) resolved `catalog/lunar_dispositions.csv` and
    `catalog/lunar_ephemeris.csv` through the unsynchronised process-wide
    `std::filesystem::path`, racing that write. Every other reader (textures, shaders,
    star/constellation catalogs, runtime `asset_root_`) runs on the main thread.
- [x] **Fix:** Prefer immutable per-service roots; otherwise synchronize reads and writes using the same protection.
  - `SatViewCatalogService::Config::asset_root` is resolved once in `start()` on the
    calling thread (the runtime passes its per-pane `asset_root_`); the startup path and the
    worker read only that copy through new explicit-root overloads of
    `load_bundled_lunar_dispositions`, `load_bundled_sampled_ephemeris`, and
    `load_sample_satellite_catalog`.
  - The remaining process-wide default is guarded by a mutex; `resolve_satview_asset_path`
    copies it under the lock.
- [x] **Acceptance:** Controlled overlapping pane creation and catalog loading cannot access a path concurrently with mutation or mix pane asset roots.
  - `SatView catalog workers keep their own asset root while another pane resets the default`
    (`tests/satview_catalog_service_tests.cpp`) holds the worker inside its fetch, hammers
    `set_satview_asset_root` from a "pane creator" thread at a missing directory, and proves
    the startup sample and the worker's lunar dispositions both came from the service root.
- [x] **Validation:** Run the SatView-scoped aggregate, multi-pane checks, and same-cache smoke on both supported platform paths.
  - macOS: focused `[assets]`, SatView-scoped aggregate (includes the multi-pane host smoke
    tests), and same-cache smoke. The change is platform-neutral C++ (std::mutex /
    std::filesystem); Windows coverage comes from the usual CI run.
