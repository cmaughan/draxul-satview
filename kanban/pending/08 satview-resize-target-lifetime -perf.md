# Retire SatView resize targets without device-wide waits

**Summary:** Replace SatView's size-dependent drawing resources after their current use finishes so resizing a pane does not repeatedly wait for all graphics work to stop.

**Source:** `plugins/satview/src/render/satview_render_vk.cpp`  
**Priority:** P2; static, medium-high confidence. **Reported by:** Claude. Lines 1057–1099 wait for device idle and recreate all frame targets on exact size changes; debug difference resources are also required when unused. Dragging a divider can repeat this work.

- [ ] **Baseline:** Count waits, target creations, allocated bytes, and resize-frame p95 through a scripted drag.
- [ ] **Implement:** Retire old size generations through completed slots and allocate debug attachments on demand.
- [ ] **Functional safety:** Preserve correct current viewport sizing, transactional allocation failure, debug toggle, and slot lifetime.
- [ ] **Compare:** Require no device-wide wait for ordinary extent changes and report peak memory.
- [ ] **Platforms:** Run Vulkan validation and compare Metal resize/output behavior, SatView aggregate and smoke.
- [ ] **Acceptance:** Resize reuses compatible resources without showing stale-sized frames.
