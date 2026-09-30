# Rebuild only lunar-dependent SatView tracks

**Source:** `plugins/satview/src/runtime/satview_runtime.cpp`  
**Priority/evidence:** P2; static, medium-high confidence. **Reported by:** Claude. Lines 1217–1226 trigger reanchoring when any lunar track is present; lines 1248–1270 recompose and upload the whole track vector, including stable Earth tracks. Other dirty/source/window gates are bypassed by that condition.

- [ ] **Baseline:** Count composed and uploaded vertices for mixed Earth/lunar tracks during time and camera changes and while paused.
- [ ] **Implement:** Separate stable and lunar-dependent geometry/revisions without an unmeasured visual tolerance.
- [ ] **Functional safety:** Preserve map wrap, selection, ground projection, Moon windows, and track invalidation.
- [ ] **Compare:** Require unchanged Earth-track work to remain flat when only lunar anchoring changes.
- [ ] **Platforms:** Check SatView output on Vulkan and Metal, aggregate and smoke.
- [ ] **Acceptance:** Lunar reanchoring does not upload unrelated stable tracks.
