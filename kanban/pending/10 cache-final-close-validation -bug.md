# Check completed cache writes before replacement

**Summary:** Check that cache files finish writing before replacing the saved copy so storage failures cannot publish incomplete data.

**Priority:** 10  
**Severity:** CRITICAL  
**Source:** `plugins/satview/src/services/satview_cache_publication.cpp`

**Evidence and trigger:** B29; catalog and cloud writers replace saved files without checking final flush or close failure, potentially publishing truncated data.

- [ ] **Investigate:** Trace both publication helpers and catalog acceptance of truncated complete-row prefixes.
- [ ] **Fix:** Explicitly close and check streams before replacement; remove only the writer’s temporary file on failure.
- [ ] **Acceptance:** Injected close-stage failure returns failure, preserves destination bytes, and cleans temporary files for catalog and cloud writes.
- [ ] **Validation:** Check both platform replacement branches; run the SatView-scoped aggregate and same-cache smoke.
