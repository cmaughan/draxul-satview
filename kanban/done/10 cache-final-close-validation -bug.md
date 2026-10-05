# Check completed cache writes before replacement

**Summary:** Check that cache files finish writing before replacing the saved copy so storage failures cannot publish incomplete data.

**Priority:** 10  
**Severity:** CRITICAL  
**Source:** `plugins/satview/src/services/satview_cache_publication.cpp`

**Evidence and trigger:** B29; catalog and cloud writers replace saved files without checking final flush or close failure, potentially publishing truncated data.

- [x] **Investigate:** Trace both publication helpers and catalog acceptance of truncated complete-row prefixes.
  - `detail::write_cache_text_atomically` (catalog payload + metadata) and a duplicated
    `write_binary_atomic` in `satview_cloud_service.cpp` both checked `out.good()` after
    `write()` and then let the `std::ofstream` destructor flush/close unchecked before
    renaming. A deferred write error (full disk, network filesystem) therefore published
    a truncated temporary. A write failure also leaked the temporary file.
  - The catalog cache reader accepts any parseable payload: a SATCAT CSV cut at a row
    boundary parses as a smaller valid catalog, and the metadata (written after the
    payload) cannot reliably disambiguate it because parser policy changes legitimately
    alter counts. The fix therefore guarantees such prefixes are never published.
- [x] **Fix:** Explicitly close and check streams before replacement; remove only the writer’s temporary file on failure.
  - `close_cache_temporary` flushes and closes and reports `fail()`; the writer refuses
    to replace the destination when it fails and removes only its own temporary on open,
    write, close, and replace failures. Temporaries now include the process id.
  - The cloud service publishes through the same shared helper; its private duplicate
    writer was removed.
- [x] **Acceptance:** Injected close-stage failure returns failure, preserves destination bytes, and cleans temporary files for catalog and cloud writes.
  - `SatView cache publication rejects a temporary whose final close fails`
    (`tests/satview_catalog_service_tests.cpp`) covers catalog payload, catalog metadata,
    and cloud image destinations via `detail::CacheFileOperations` injection, then proves
    the production close publishes the complete payload.
- [x] **Validation:** Check both platform replacement branches; run the SatView-scoped aggregate and same-cache smoke.
  - Replacement branches are unchanged (`MoveFileExW` on Windows, `std::filesystem::rename`
    elsewhere) and still covered by the existing macOS/Windows replacement-failure tests;
    the close check runs before either branch. macOS: focused `[cache],[cloud]`,
    SatView-scoped aggregate, and same-cache smoke; Windows via CI.
