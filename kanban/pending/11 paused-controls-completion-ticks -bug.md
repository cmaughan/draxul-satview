# Process controls and completed work while paused

**Summary:** Keep paused satellite panes processing controls and completed downloads so changes can appear without resuming time.

**Priority:** 11  
**Severity:** HIGH  
**Source:** `plugins/satview/src/satview_plugin.cpp`

**Evidence and trigger:** B20; paused ticks remove their deadline, while worker completion and redraw-only input do not schedule another pump.

- [ ] **Investigate:** Trace adapter scheduling for completion, refresh, camera keys, visibility, and pause.
- [ ] **Fix:** Request completion/control ticks and bounded active-control deadlines without advancing paused simulation time.
- [ ] **Acceptance:** Paused startup and refresh consume completed results; camera controls work; inactive panes settle without busy loops.
- [ ] **Validation:** Exercise the real plugin adapter scheduling boundary; run the SatView-scoped aggregate and same-cache smoke.
