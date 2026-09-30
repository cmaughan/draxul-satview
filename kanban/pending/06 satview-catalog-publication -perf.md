# Load cached SatView catalogs without blocking pane creation

**Source:** `plugins/satview/src/services/satview_catalog_service.cpp`  
**Priority/evidence:** P2; static, high confidence. **Reported by:** Claude. Lines 317–417 read and parse GP/SATCAT caches synchronously; `satview_runtime.cpp:826–827` transfers a by-value catalog onward. This affects opening or restoring panes; refresh workers do not protect this initial cached-load path.

- [ ] **Baseline:** Measure create-instance GUI time, file reads, and catalog copies with populated caches and one/multiple panes.
- [ ] **Implement:** Publish an immutable catalog from one owned worker with a bounded latest result and loading presentation.
- [ ] **Functional safety:** Preserve corrupt-cache diagnostics, network fallback, settings changes, and worker shutdown.
- [ ] **Compare:** Require no catalog file reads on the GUI creation path and report startup latency/copies.
- [ ] **Platforms:** Check SatView startup on Windows and macOS, aggregate and smoke.
- [ ] **Acceptance:** A large cached catalog does not block pane creation.
