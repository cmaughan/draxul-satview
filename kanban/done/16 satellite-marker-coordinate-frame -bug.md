# Restore satellite markers to the rendered Earth coordinate frame

**Summary:** Satellite markers use raw orbital axes after scene extraction, so they no longer align with Earth, their tracks, or picking.

**Priority:** P1 — satellites appear at incorrect geographic positions and visual selection disagrees with picking.

- [x] Trace the marker, Earth, track and picking coordinate contracts and identify the introducing change.
- [x] Restore `teme_position_to_render_earth_radii()` for both marker interpolation endpoints in `compose_satview_markers()`.
- [x] Add explicit cardinal-axis, north-pole, interpolation and marker/track alignment regressions; correct existing composer fixtures that assume raw orbital axes are render coordinates.
- [x] Verify ground-horizon visibility and marker sizing use the converted positions and agree with picking.
- [x] Run the SatView aggregate and same-cache smoke, inspect Vulkan globe/map output, inspect Metal consumption, and confirm Release startup. Deterministic cardinal positions and marker/track alignment are asserted by the CPU contract tests; current GPU capture configuration does not expose fixed simulation/catalog injection. Automated GPU fixture work is explicitly tracked in `17 satellite-render-regression-fixture -test.md` (done).

## Diagnosis — 2026-10-08

- Introducing SatView commit: `8468698591b397a1ce0a039248c9a9dc370961d4`
  (`Harden SatView services and extract scene boundaries`, 2026-09-21).
  Before extraction, `satview_runtime.cpp` converted both the current position
  and `next_teme_position(...)` with `teme_position_to_render_earth_radii()`.
- `src/scene/satview_scene_composer.cpp:182` and `:203` now only divide
  `teme_position_km` and the next position by Earth's radius. That changes units
  but omits the coordinate-frame conversion. The request still contains actual
  TEME positions, not pre-converted render vectors.
- The established conversion in `src/core/satview_propagation.cpp:1034` is
  `(x, y, z) / R -> (-y, z, -x) / R`: orbital +Z is render +Y (north).
  This is a missing axis permutation/sign conversion, not a matrix transpose
  or simply exchanging X and Y. A north-pole marker `(0, 0, R)` is emitted at
  `(0, 0, 1)` instead of `(0, 1, 0)`; a TEME +X marker appears at render +X
  instead of -Z.
- `shaders/satview_marker.vert` interpolates the supplied positions and treats
  them directly as render coordinates. Track vertices already use converted
  `render_teme_points_earth_radii`, and picking still calls the proper helper
  in `src/runtime/satview_runtime.cpp:3019`. Horizon filtering and ground marker
  sizing also incorrectly compare unconverted marker positions to a render-space
  observer. The CPU scene composer is shared by Vulkan and Metal.
- Existing propagation tests validate the correct north-pole mapping, but the
  scene-composer tests use raw-axis fixtures and assert increasing TEME X becomes
  increasing render X. They do not test the boundary against the propagation
  helper or the corresponding track. This explains how both layers' tests could
  pass independently while the assembled scene is wrong.
- This establishes the satellite-marker regression. No separate Earth texture,
  globe-axis, or polar-landmark regression is yet proven; a screenshot/description
  of the user's pole observation will distinguish it from misplaced markers.
- Diagnostic run of the already-built Debug executable with
  `[satview][coordinates],[satview][composer]`: all 9 cases / 45 assertions pass,
  0.90 s command wall time. This confirms the coverage gap, not correctness of
  the broken boundary. No configure/build, aggregate, GPU capture, or startup
  check was needed for this source-only investigation.

## Repair and regression coverage

- Restored the original conversion at the two marker endpoints. No shader,
  Earth texture, orbit propagation, camera, or radius-only distance calculation
  changed. Both Vulkan and Metal consume the corrected shared scene records.
- Explicit expected vectors cover all six cardinal axes plus an asymmetric
  position. Marker endpoints match track vertices; interpolation at 0, 0.35,
  and 1 matches the coordinate path used by picking. Missing next samples retain
  the correctly converted current position.
- Geographic cases cover the equator, northern/southern latitudes, 80 degrees
  north, and rotated map centres. They check both independent longitude/latitude
  expectations and the marker shader's inverse-axis convention against CPU map
  picking. These are CPU contract tests, not GPU execution or mouse-hit tests.
- Corrected old raw-axis test assumptions and checked horizon eligibility and
  observer-relative marker size at a short overhead range. Documented the frame
  contract in `AGENTS.md` to protect future extraction work.
- Red test before production repair: composer selection, 9 cases, 4 failed;
  72/129 assertions failed in 0.14 s. Focused test-target build took 10.70 s.
  Log: `build-ninja-debug/windows-gates/satview-coordinate-before.log`.
  This proves the regression checks detect the original bug rather than merely
  passing the corrected code.
- Core + SatView aggregate: 54/59 entries passed in 112.24 s (runner 112.41 s).
  All SatView groups passed: C++ shards 49.09/23.85 s and catalog Python 1.99 s.
  Five known core failures remain: personal schedule cleanup, family emoji,
  remote initial-state timing, test-scope integration, and personal-host Unicode
  image paths. None were waived or changed by this repair.
- Debug aggregate build: 15 steps, 20.48 s in the existing cache. Paired fresh-
  profile same-cache Debug smoke passed, exit 0 in 7.527 s including environment
  setup. The test-owned server was shut down; the existing user server was
  untouched. Release incremental build passed, 11 steps in 20.14 s, no configure.
- Logs: `build-ninja-debug/windows-gates/satview-coordinate-aggregate.log`,
  `satview-coordinate-ctest.log`, `satview-coordinate-debug-smoke.log`,
  `satview-coordinate-release-build.log`.

## Completion evidence and limitations

- Final same-cache Release smoke passed, exit 0 in 4.822 s including environment
  setup; log `satview-coordinate-release-smoke.log`. Both test-owned startup
  servers were stopped. The user's original server was left untouched.
- Vulkan globe and map frames render the real Earth. The empty-filter map
  capture visibly contains satellite markers and their colored tracks, including
  matching orange and cyan markers on their respective curves. This is real
  GPU overlay smoke evidence, not a deterministic geographic-position proof;
  exact pole/axis/interpolation/map agreement is established by the CPU tests.
- Visual costs: globe comparison 11.438 s (failed against an obsolete stylized
  reference, 42.8625% difference); STARLINK map export 24.438 s (exit 0 but no
  matching markers); unfiltered map export 25.406 s (exit 0 with visible markers
  and tracks). One warmed 16,683-object unpaused STARLINK retry exceeded its
  30 s deadline (30.359 s including exact-process cleanup), produced no frame,
  and was not retried. None of these outcomes is hidden by a reference blessing.
- Visual artifacts: `build-ninja-debug/windows-gates/satview-coordinate-smoke/`.
  Its stderr also contains existing Vulkan/loader diagnostics; this CPU-only
  repair does not claim a globally validation-clean renderer. The stale reference
  and missing controlled GPU fixture are owned by
  `17 satellite-render-regression-fixture -test.md` (done); paused publication
  remains owned by `../pending/11 paused-controls-completion-ticks -bug.md`.
- Metal's `satview_marker_vertex` interpolates the same scene endpoints directly,
  so the shared CPU correction applies without backend changes. No macOS runtime
  or remote CI was run here; normal cross-platform CI remains applicable.
- Closed the coordinate repair on failing-before/passing-after regressions,
  passing complete SatView groups, paired Debug/Release startup and inspected
  Vulkan overlay output. Aggregate and visual-harness limitations remain tracked
  separately. No production shader changes, reference blessings or commits.
