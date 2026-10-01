# Give background loaders safe asset locations

**Summary:** Protect the shared asset location so opening another satellite pane cannot interfere with background file loading.

**Priority:** 09  
**Severity:** CRITICAL  
**Source:** `plugins/satview/src/core/satview_texture_assets.cpp`

**Evidence and trigger:** B02; pane creation modifies a shared path while existing catalog workers resolve bundled files.

- [ ] **Investigate:** Trace every asset-root read and write across multiple panes, loading, and shutdown.
- [ ] **Fix:** Prefer immutable per-service roots; otherwise synchronize reads and writes using the same protection.
- [ ] **Acceptance:** Controlled overlapping pane creation and catalog loading cannot access a path concurrently with mutation or mix pane asset roots.
- [ ] **Validation:** Run the SatView-scoped aggregate, multi-pane checks, and same-cache smoke on both supported platform paths.
