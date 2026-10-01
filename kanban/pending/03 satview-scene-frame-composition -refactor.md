# Put SatView surface projection and frame description in scene

**Summary:** Use the same placement and visibility calculations for drawing and selecting SatView objects so clicking an object agrees with where it appears.

**Priority:** P2 — draw and picking repeat visibility/position choices.  
**Source:** `plugins/satview/src/runtime/satview_runtime.cpp`  
**Proposed by:** Claude 25; Codex 7. **Owner:** one SatView scene agent; serialize with card 02.  
**Evidence:** runtime helpers compose surface markers while picking repeats visibility, map wrap and placement; draw fills many pass setters.

**Boundary verification**
- [ ] Pin selected overrides, representatives, Moon/Mars placement, wrap and stream revision rules.
**Implementation and migration**
- [ ] Add value-only surface request/projection in `draxul-satview-scene`; make draw and picking consume it. Then replace related setter groups with a frame description, preserving partial upload revisions.
**Unit tests**
- [ ] Device-free surface and frame/revision cases, including wrapped picking.
**Cross-platform validation**
- [ ] Verify Vulkan/Metal SatView scenarios, `--satview` aggregate and smoke.
**Agent documentation and tooling**
- [ ] Update scene/runtime ownership guide.
**Acceptance criteria**
- [ ] One projection drives visual placement and identity selection without an all-at-once draw rewrite.
