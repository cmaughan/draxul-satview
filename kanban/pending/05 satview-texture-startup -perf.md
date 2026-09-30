# Remove blocking SatView texture setup from first render

**Source:** `plugins/satview/src/render/satview_render_vk.cpp`  
**Priority/evidence:** P2; static, high confidence. **Reported by:** Claude. Lines 515–545 decode and immediately upload Earth, Moon, Sun, and Milky Way images during first render; Metal has analogous synchronous setup at `satview_render.mm:135–151`. Vulkan cloud replacement also waits idle at line 681. Actual decoded bytes and stall duration remain unmeasured.

- [ ] **Baseline:** Record actual decoded dimensions/bytes, first-pane frame time, upload waits, and forced cloud-refresh hitch for one and two panes.
- [ ] **Implement:** Decode off-thread and publish bounded frame-slot uploads, using existing solid fallbacks until ready.
- [ ] **Functional safety:** Preserve cancellation, slot retirement, resource failure, and completed Metal cloud-refresh lifetime behavior.
- [ ] **Compare:** Report first-frame and refresh stalls and peak memory before/after; assess mips/compression separately.
- [ ] **Platforms:** Verify images and cloud updates on Vulkan and Metal, SatView aggregate and smoke.
- [ ] **Acceptance:** Image decode and per-image immediate waits no longer block first-frame presentation.
