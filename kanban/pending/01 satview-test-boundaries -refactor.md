# Separate SatView CPU, scene and native tests

**Priority:** P2 — catalog/scene cases inherit runtime, renderer, SDL and Metal shader build.  
**Source:** `plugins/satview/cmake/Tests.cmake`  
**Proposed by:** Claude 49; Codex 5. **Owner:** one SatView test agent. **Depends on:** root card 02.  
**Evidence:** all C++ suites are globbed into one target; core, scene and services already have separate libraries.

**Boundary verification**
- [ ] Classify catalog, geodetic, scene, service, asset/shader and native cases.
**Implementation and migration**
- [ ] Add explicit CPU/scene/service targets and retain runtime/GPU target and Python catalog test in product aggregate.
**Unit tests**
- [ ] Preserve full case inventory and prove focused target closure.
**Cross-platform validation**
- [ ] Keep Metal texture tests and shader compilation native; run `--satview` aggregate and smoke on both OSes.
**Agent documentation and tooling**
- [ ] Update SatView focused test guidance.
**Acceptance criteria**
- [ ] CPU cases build without runtime renderer; scene may still inherit current render-contract headers until root card 21.
