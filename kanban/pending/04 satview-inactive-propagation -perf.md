# Stop settled inactive satellite propagation

**Summary:** Stop repeatedly calculating satellite positions once a paused or hidden SatView has settled so inactive panes do not keep using processor time.

**Source:** `plugins/satview/src/runtime/satview_simulation_worker.cpp`  
**Priority:** P1; static, high confidence. **Reported by:** Claude, Codex. Lines 310, 376, and 476 retain a 16 ms propagation loop; pause suppresses only the future sample, and `satview_plugin.cpp:272–282` does not pass visibility to the worker. Model and track caches still leave current-position propagation.

- [ ] **Baseline:** Count propagation calls, publications, and worker CPU with a fixed catalog while visible, paused, and hidden.
- [ ] **Implement:** Publish a settled state, then wait for catalog, clock, settings, resume, visibility, or shutdown changes; preserve resume clock anchoring.
- [ ] **Functional safety:** Check settings while paused, restored settings, pause authority, output after reveal, and bounded worker shutdown.
- [ ] **Compare:** Require zero propagations after an unchanged paused or hidden state settles.
- [ ] **Platforms:** Run SatView aggregate, render checks, and same-cache smoke on Windows/Vulkan and macOS/Metal.
- [ ] **Acceptance:** Inactive work stops without delaying a real state change.
