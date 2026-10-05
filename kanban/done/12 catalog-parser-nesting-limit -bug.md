# Bound catalog parsing depth
**Summary:** Reject excessively nested catalog data so a malformed download or saved file cannot exhaust the application’s stack.

**Priority:** 12  
**Severity:** CRITICAL  
**Source:** `plugins/satview/src/core/satview_catalog.cpp`  
**Reported by:** Claude M13; consensus F21.

**Evidence and trigger:** Lines 274, 296, and 314 recursively skip object/array values without a depth cap. Deep nesting inside an object field reaches this path before record validation.

- [x] **Investigate:** Trace network, startup-cache, and bundled-file parser entry points and failure publication.
  - All three reach `JsonReader` through `parse_celestrak_gp_json`: the refresh worker
    (network payload, on a small worker stack), `read_cache_files` in
    `SatViewCatalogService::start` (startup cache, main thread), and
    `load_sample_satellite_catalog` (bundled sample). Ignored nested values recursed
    through `parse_value` → `skip_object`/`skip_array` with no bound.
- [x] **Fix:** Enforce a bounded nesting depth for both arrays and objects and return useful diagnostics.
  - `JsonReader::enter_container` counts every array/object (outer record array and
    record objects included) against `kSatViewCatalogJsonMaxNestingDepth` (64, in
    `satview_catalog.h`) before recursing and fails with
    `JSON nesting exceeds 64 levels at byte N`.
- [x] **Acceptance:** Excessive nesting fails safely without stack exhaustion; permitted boundary depth parses normally.
  - `SatView GP JSON parser bounds nesting depth` (`tests/satview_catalog_tests.cpp`):
    depth 64 arrays/objects parse, depth 65 fails with the diagnostic, and 2,000,000-deep
    (terminated and unterminated) documents fail without crashing.
- [x] **Acceptance:** Failed refresh retains usable catalog state and startup failure follows the intended fallback.
  - `SatView catalog service rejects excessively nested GP JSON at every entry point`
    (`tests/satview_catalog_service_tests.cpp`): a 200,000-deep network refresh keeps the
    cached catalog and cache bytes and reports the error; the same payload in the startup
    cache falls back to the bundled sample.
- [x] **Validation:** Run the SatView-scoped aggregate and same-cache smoke.
  - macOS: focused `[json]`, SatView-scoped aggregate, and same-cache smoke; Windows via CI.
