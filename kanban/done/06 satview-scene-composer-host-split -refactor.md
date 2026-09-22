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
- [x] Move remaining track and visible-radius helpers with output-equivalence tests.
- [x] Replace direct marker vector assembly with one composer request/result incrementally.
- [x] Add a private view/selection controller for POV, map/ground interactions, and selection transitions.
- [x] Move ImGui windows/object-tree methods into a private host panel TU.
- [x] Keep services, worker publication, renderer attachment, frame scheduling, and dirty orchestration in `SatViewHost`.
- [x] Do not add another static library or change the renderer scene ABI.

## Unit tests

- [x] Add device-free composer tests for marker limits, filters, selected emphasis, and visible bounds.
- [x] Pin composer generation behavior and ground/map eligibility variants.
- [x] Add controller tests for POV changes, map dragging, ground entry, and selection/clear transitions.
- [x] Retain the offline host smoke fixture for integrated lifecycle/draw behavior.
- [x] Build `draxul-satview-scene` and `draxul-test-satview`; run CTest label `satview`.

## Cross-platform validation

- [x] Configure/build SatView ON and OFF on Windows.
- [x] Configure/build SatView ON and OFF on macOS.
- [x] Verify Vulkan and Metal consume the unchanged scene records and revision semantics.
- [x] Ensure composer contains no HTTP, ImGui, SDL, Vulkan, Metal, or GPU-resource ownership.
- [x] Run the host on Metal and validate the Vulkan runtime on Windows.

## Agent documentation and tooling

- [x] Update the root `docs/module-map.md` with scene collaborator ownership.
- [x] Add a SatView nested guide with collaborator ownership.
- [x] Document the immutable composer request/result and main-thread publication rules.
- [x] Ensure `python3 do.py test debug --satview` remains the narrow validation entry point.

## Acceptance criteria

- [x] Scene construction and interaction tests run without a GPU, window, network, or system clock.
- [x] `SatViewRuntime` retains one owner for service lifetime, frame scheduling, and publication.
- [x] Renderer ABI, visuals, config persistence, and dirty-frame behavior are unchanged.
- [x] Independent scene, interaction, and panel ownership lanes no longer require editing one giant TU.
- [x] Focused/full tests and same-cache smoke pass.

## Dependencies and ownership

Depends on the core repository's internal-target build-policy work. One SatView
owner freezes request/result and state ownership. Composer tests and panel
TU moves may then be independent; renderer backend files remain single-owner and
out of scope.

## Validation evidence

- 2026-09-22 Windows Ninja Debug: `py do.py test debug --satview` passed all
  27 selected core and SatView CTest entries after the composer, controller, and
  panel translation-unit extraction.
- 2026-09-22 Windows Ninja Debug: the post-extraction products aggregate passed
  all 48 CTest entries in 230.53 seconds, followed by a successful same-cache
  smoke check.
- 2026-09-22 Windows Vulkan: the SatView developer render exported a valid
  960x640 frame and the result was visually inspected.
- A fresh isolated MSVC/Ninja `build-validation/satview-on-msvc` cache was
  configured with `DRAXUL_ENABLE_SATVIEW=ON` and every other product OFF.
  Building `draxul`, `draxul-satview-scene`, and `draxul-test-satview`
  passed; `ctest -L satview --parallel 8` then passed 3/3 entries in 37.60
  seconds.
- A separate `build-validation/products-off-msvc` cache was configured with
  both `DRAXUL_ENABLE_SATVIEW=OFF` and `DRAXUL_ENABLE_MEGACITY=OFF`, with
  every other product also OFF. Building `draxul` passed, and target
  enumeration confirmed that no SatView target was present.

## macOS closeout

Completed on 2026-09-22 from the Draxul root with separate isolated caches for
the explicit feature matrix:

```bash
env CCACHE_DIR=/tmp/draxul-ccache cmake -S . -B build-mac-satview-on -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Debug -DDRAXUL_ENABLE_RENDER_TESTS=ON \
  -DDRAXUL_ENABLE_MEGACITY=OFF -DDRAXUL_ENABLE_SATVIEW=ON \
  -DDRAXUL_ENABLE_SCOREVIEW=OFF -DDRAXUL_ENABLE_PCBVIEW=OFF \
  -DDRAXUL_ENABLE_REZONALITY=OFF -DDRAXUL_REQUIRE_ENABLED_PLUGINS=ON \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DFETCHCONTENT_BASE_DIR="$PWD/build-mac-satview-on/_deps"
env CCACHE_DIR=/tmp/draxul-ccache cmake --build build-mac-satview-on --target draxul \
  draxul-satview-scene draxul-test-satview --parallel 8
ctest --test-dir build-mac-satview-on -L satview --output-on-failure --parallel 8

env CCACHE_DIR=/tmp/draxul-ccache cmake --build build-mac-products-off \
  --target draxul --parallel 8
cmake --build build-mac-products-off --target help | rg -i 'draxul-satview|satview'
find build-mac-products-off/draxul.app -iname '*satview*' -print

build-mac-satview-on/draxul.app/Contents/MacOS/draxul --console \
  --render-test "$PWD/tests/render/satview-plugin.toml" \
  --show-render-test-window --export-render-test /tmp/draxul-satview-macos.bmp

build-mac-satview-on/draxul.app/Contents/MacOS/draxul --console --smoke-test
```

- The isolated ON configure used the existing disconnected dependency sources,
  built `draxul`, `draxul-satview-scene`, and `draxul-test-satview`, and
  `ctest -L satview` passed 3/3 entries in 3.40 seconds.
- The existing products-OFF cache built `draxul`; target enumeration found no
  SatView build target and the application bundle contained no SatView payload.
- The Metal developer render, bounded by the command runner, used
  `tests/render/satview-plugin.toml`, exited successfully on Apple M5 in 7.20
  seconds, exported a valid 960x640 frame, and the capture was visually
  inspected. The matching ON-cache smoke exited successfully in 5.16 seconds.
- The passing controller tests cover map dragging, ground entry, POV changes,
  and mutually exclusive selection transitions. The passing host and label
  tests cover the integrated offline lifecycle, selection clearing, frame
  request settling and dirty revisions, and label layout. Together with the
  inspected Metal frame, these provide the non-interactive transition and
  rendering evidence for the macOS closeout.
