# Reject non-finite satellite settings
**Summary:** Reject invalid numeric settings so the satellite view keeps usable lighting and ground coordinates.

**Priority:** 14  
**Severity:** MEDIUM  
**Source:** `plugins/satview/src/runtime/satview_config_io.cpp`  
**Reported by:** Claude M15; consensus F51.

**Evidence and trigger:** NaN survives the clamp at line 55; infinite longitude becomes NaN at line 235. Lighting and ground calculations receive these invalid values.

**Related:** `plugins/satview/kanban/pending/02 satview-configuration-ownership -refactor.md`.

- [ ] **Investigate:** Trace saved/launch configuration and direct runtime application into lighting, projection, and simulation controls.
- [ ] **Fix:** Reject non-finite values before clamping or wrapping and retain valid defaults or previous state.
- [ ] **Acceptance:** NaN and infinities cannot reach affected rendering/ground calculations through configuration.
- [ ] **Acceptance:** Finite boundary settings retain documented clamping and wrapping behavior.
- [ ] **Validation:** Run the SatView-scoped aggregate, affected view checks, and same-cache smoke.
