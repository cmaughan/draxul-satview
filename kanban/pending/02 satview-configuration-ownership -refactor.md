# Make SatView persistent configuration authoritative

**Priority:** P2 — roughly fifty settings are mirrored between runtime fields and config.  
**Source:** `plugins/satview/src/runtime/satview_runtime.cpp`  
**Proposed by:** Claude 25, with part of 48. **Owner:** one SatView runtime agent; serialize with card 03.  
**Evidence:** `current_config()` copies member fields out; `apply_config()` copies them back and triggers worker effects. Adapter retains unused triangle state.

**Boundary verification**
- [ ] Separate persistent settings from transient selection, camera, worker and pause authority.
**Implementation and migration**
- [ ] Store one normalized `SatViewConfig`; migrate coherent groups/panels and derive side effects from diff. Remove unused triangle fields/current-format serialization.
**Unit tests**
- [ ] Round trip, normalization, restored worker settings, pause and adapter storage.
**Cross-platform validation**
- [ ] Run `--satview` aggregate, native render cases and same-cache smoke on both OSes.
**Agent documentation and tooling**
- [ ] Update product configuration ownership notes.
**Acceptance criteria**
- [ ] No mirrored persistent setting remains; completed restore/pause regressions stay fixed.
