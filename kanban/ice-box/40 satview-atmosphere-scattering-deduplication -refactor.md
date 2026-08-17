# Reconcile duplicated atmosphere-scattering math

**Type:** refactor

## Gap

SatView retains three similar atmosphere-scattering implementations. They differ in
step counts, ray-start policy, scattering multiplier, and night tinting, so mechanical
deduplication would hide meaningful policy behind a large parameter list.

## Work

- [ ] Inventory the physical invariants and intentional view-specific policy.
- [ ] Extract only the genuinely identical math or document why separate kernels are
      easier to verify.
- [ ] Compare Vulkan/Metal output and the affected SatView render scenarios before and
      after any consolidation.
