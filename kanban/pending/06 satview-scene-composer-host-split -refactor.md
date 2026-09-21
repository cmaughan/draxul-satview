# Extract SatView scene composition and host interaction collaborators

**Type:** refactor
**Priority:** P1 / sequence 06
**Raised by:** Claude and GPT/Codex
**Consensus:** `plans/reviews/review-refactor-consensus.md`, Accepted 7

## Goal

Move backend-neutral scene composition into the existing scene target and split
view/selection and ImGui responsibilities inside the host target, leaving
`SatViewHost` as lifecycle/frame orchestration.

## Boundary verification

- [x] Classify the pure helpers, `draw()` assembly, config/dirty state, picking,
  camera transitions, panels, object tree, and service/worker lifetime in `satview_host.cpp`.
- [x] Inventory existing scene POD contracts and renderer consumption on Vulkan/Metal.
- [x] Record dirty-buffer revisions, generation checks, visible-radius, and selection invariants.
- [x] Record offline fixture seams for fake clock, transport, renderer, and callbacks.
- [x] Confirm the proposal is narrower than the delivered SatView library-boundary
  refactor retained in repository history.

## Implementation and migration

- [x] Add `SatViewSceneComposer` request/result values to `draxul-satview-scene`.
- [x] Move pure marker/color/filter/limit helpers with output-equivalence tests.
- [ ] Move remaining track and visible-radius helpers with output-equivalence tests.
- [x] Replace direct marker vector assembly with one composer request/result incrementally.
- [ ] Add a private view/selection controller for POV, map/ground interactions, and selection transitions.
- [ ] Move ImGui windows/object-tree methods into private host panel TUs one window at a time.
- [x] Keep services, worker publication, renderer attachment, frame scheduling, and dirty orchestration in `SatViewHost`.
- [x] Do not add another static library or change the renderer scene ABI.

## Unit tests

- [x] Add device-free composer tests for marker limits, filters, selected emphasis, and visible bounds.
- [x] Pin composer generation behavior and ground/map eligibility variants.
- [ ] Add controller tests for POV changes, map dragging, ground entry, and selection/clear transitions.
- [x] Retain the offline host smoke fixture for integrated lifecycle/draw behavior.
- [x] Build `draxul-satview-scene` and `draxul-test-satview`; run CTest label `satview`.

## Cross-platform validation

- [ ] Configure/build SatView ON and OFF on Windows and macOS.
- [x] Verify Vulkan and Metal consume the unchanged scene records and revision semantics.
- [x] Ensure composer contains no HTTP, ImGui, SDL, Vulkan, Metal, or GPU-resource ownership.
- [x] Run the host on Metal; Vulkan runtime validation remains pending on Windows.

## Agent documentation and tooling

- [ ] Update the root `docs/module-map.md` with scene collaborator ownership.
- [x] Add a SatView nested guide with collaborator ownership.
- [x] Document the immutable composer request/result and main-thread publication rules.
- [x] Ensure `python3 do.py test debug --satview` remains the narrow validation entry point.

## Acceptance criteria

- [ ] Scene construction and interaction tests run without a GPU, window, network, or system clock.
- [x] `SatViewRuntime` retains one owner for service lifetime, frame scheduling, and publication.
- [x] Renderer ABI, visuals, config persistence, and dirty-frame behavior are unchanged.
- [ ] Independent scene, interaction, and panel ownership lanes no longer require editing one giant TU.
- [ ] Focused/full tests, optional ON/OFF builds, and smoke pass.

## Dependencies and ownership

Depends on the core repository's internal-target build-policy work. One SatView
owner freezes request/result and state ownership. Composer tests and panel
TU moves may then be independent; renderer backend files remain single-owner and
out of scope.
