# Make the SatView render regression exercise real satellite placement

**Summary:** Replace the obsolete stylized reference with a controlled real SatView scene so render checks can catch satellite coordinate regressions.

**Priority:** P1 — the existing visual gate neither matches the shipped scene nor reliably exercises satellite markers.

- [x] Record the failed existing golden comparison and inspect the actual/reference images.
- [x] Provide test-only fixed time, camera and offline catalog/state injection through the real plugin rendering path, without adding user-facing fixture configuration.
- [x] Exercise known satellite positions and matching tracks in globe and map views, including geographic poles and the equator; require markers to be present.
- [x] Establish reviewed Windows references using the repository blessing workflow and retain Metal-compatible scene contracts.
- [x] Run render scenarios, SatView aggregate and same-cache smoke with retained costs/results.

## Evidence — 2026-10-08

While validating `../done/16 satellite-marker-coordinate-frame -bug.md`, the existing
`tests/render/satview-plugin.toml` run failed with 42.8625% changed pixels
(263,347 pixels; threshold 0.05%). The reference
`tests/render/reference/satview-plugin.windows.bmp` depicts a stylized globe and
orbit dots; the actual scene has the real textured Earth and constellation labels.
This is not a useful marker-coordinate acceptance comparison. No reference was
blessed to hide the difference.

The current scenario pauses simulation but does not inject a fixed clock or
catalog. An isolated STARLINK map capture rendered the Earth successfully but
contained no satellite markers, so exit 0 alone cannot establish overlay
correctness. Existing CPU hooks can supply fixed time/data, but are not wired
through this plugin/GPU capture path. Keep the test seam distinct from the
production configuration and from the separately tracked paused-completion
issue in `11 paused-controls-completion-ticks -bug.md`.

An unfiltered map export did display markers and tracks successfully (25.406 s),
but that is not controlled pole/epoch regression coverage. A single warmed-cache
unpaused STARLINK retry hit the 30 s bound and produced no frame. Its exact owned
process was cleaned up; no extra retries or user-profile modifications followed.

Local artifacts: `build-ninja-debug/windows-gates/satview-coordinate-smoke/`,
including `globe.report.json`, actual/diff BMPs, map captures, and command logs.
The coordinate repair itself has independent cardinal-axis, interpolation,
map/picking and marker/track CPU regressions; this card owns the missing
end-to-end visual regression coverage, not that production repair.

## Controlled fixture and references — 2026-10-08

### What changed and why

The old `satview-plugin` scene paused simulation but read the wall clock, the
user's catalog cache or network, and a sun-derived camera, so its stylized
reference could neither match the shipped scene nor prove markers were drawn.
It is replaced in place by a controlled scene, plus a map companion:

- `tests/render/satview-plugin.toml` (globe) and `satview-plugin-map.toml` (map)
  are Windows `regression` scenarios in `tests/render/manifest.json` with
  `test_scope: "satview"`, `requires_target: "draxul-satview-plugin"`, CTest
  names `draxul-render-satview-plugin` / `draxul-render-satview-plugin-map`, and
  `py do.py satviewplugin|satviewmap|blesssatviewplugin|blesssatviewmap`.
  They run under `do test --satview`, not the core `validate` render list.
- Offline payloads `plugins/satview/tests/fixtures/render/satview_render_fixture_gp.json`
  and `..._satcat.csv`: seven synthetic NORAD 99000x objects at epoch
  2026-03-20T12:00:00Z (GMST 358.034 degrees), mean motion 15.2 rev/day
  (~507 km): north/south polar (i=90, 0.5 degrees from each pole so the map
  longitude is defined: 89.5N 0E and 89.5S 90E), equator at 0E (i=51.6),
  90E (i=0), 180E (i=97.5), 90W (i=28.5), and 45N 45E (i=60). SATCAT rows
  vary population (active/inactive payload, rocket body, debris, unknown) so
  colours distinguish north/south and east/west swaps.
- The obsolete stylized `tests/render/reference/satview-plugin.windows.bmp` is
  replaced by the new controlled globe reference; `satview-plugin-map.windows.bmp`
  is new. The old macOS-capable developer entry is now Windows-only because a
  required reference must exist per listed platform.

### Seam design (test-only, distinct from production configuration)

- Plugin launch JSON key `render_test_fixture` (parsed in
  `src/satview_plugin.cpp`) names the payload files (`${PROJECT_ROOT}`-expanded
  by the render harness), `unix_seconds`, a globe camera look-at
  (`camera_longitude_degrees`, `camera_latitude_degrees`,
  `camera_distance_earth_radii`), `map_center_degrees`, `marker_scale` (1-8),
  `required_markers` and `required_tracks`. Every field is required and
  range-checked; files are size-bounded; any error logs and fails pane
  creation. It is not a SatView preference: never read from or written to
  `config.toml`/pane state, no UI, forces `paused` and `remember_state=false`,
  and hides the control panels (their live timings/cache ages are not part of
  the contract). View settings still come from the existing
  `satview_config_toml` (`projection_mode`, `clouds = false`).
- `SatViewRuntime::install_render_test_fixture()` (public, documented
  test-only, refused once running) reuses the existing kanban-18 `TestHooks`:
  fixed clock, fake catalog fetch serving the payloads, offline cloud stub, and
  a private temp cache directory (`draxul-satview-render-fixture-*`, removed
  when the instance is destroyed after the runtime joins its workers), so the
  real catalog service, merge, SGP4 worker, scene composer and Vulkan/Metal
  scene pass all run unchanged. The simulation worker starts already paused,
  so its clock never drifts past the epoch, and the camera/map centre are
  pinned via the same TEME-to-render conversion as markers and tracks.
- `render_test_fixture_ready()` is true only when GP and SATCAT are `Live`
  (fixture fetch, not the bundled sample or a cache), the drawn snapshot is
  paused at exactly the epoch for the current catalog generation, and the last
  draw uploaded at least `required_markers` markers and `required_tracks`
  tracks from that generation. The plugin reports `content_ready = 0` until
  then, and the render harness waits for content readiness, so a capture with
  no markers times out and fails. While pending, the fixture keeps a frame-rate
  tick deadline; once ready it idles like any paused pane. This is scoped to
  the fixture and does not address `11 paused-controls-completion-ticks -bug.md`.
- `SatViewMarkerComposeRequest::marker_scale` (default 1, production never sets
  it) multiplies globe/map marker size; ground markers keep their own scale.
  The fixtures use 6 so each marker is a clearly visible cross (production
  markers are about 1-2 px in this framing). Scene record layout is unchanged,
  so the Metal path consumes it identically; no backend file was edited (the
  existing uncommitted HDR-target retirement edits in `satview_render.mm` and
  `satview_render_vk.cpp` predate this work and were left untouched).
- Captures are bit-identical run to run, so the scenarios use
  `changed_pixels_threshold_pct = 0.005` (about 31 px of 614,400) with
  `pixel_tolerance = 4`; a single displaced enlarged marker exceeds it.

### Evidence

- CPU `[render-fixture]` tests (`tests/satview_render_fixture_tests.cpp`,
  composer scale case in `satview_scene_composer_tests.cpp`): 5 cases,
  97 assertions, 5.89 s. They assert propagated geography at the epoch
  (observed: 89.5105N 0.00002E; 89.5126S 90E; 0.076S 0.061W; 0N 90E;
  0.122S 179.984W; 0.028S 90.052W; 44.956N 44.966E), each marker lies within
  0.01 Earth radii of its own sampled track, the seam starts paused at the
  epoch with panels hidden and the camera at 0N 45E / 3.6 radii, readiness
  reaches 7 markers + 7 tracks in globe and map, the fixture cache is private,
  and readiness never arrives with markers hidden (`tracks_only`) or with one
  marker fewer than required, nor after a late install.
- Inspection (BMP to PNG with Pillow, plus markers-only diagnostic variants not
  committed): globe markers at N pole (480,181) and 45N45E (480,195) above/on
  the disc top, 0E (352,320) and 90E (607,320) on the equator limbs, S pole
  (480,462); 180E and 90W correctly occluded. Map markers at 0E (480,320),
  90E (714,320), 90W (245,320), 180E split at the left edge, 45N45E
  (597,183), N pole top edge at 0E, S pole bottom edge at 90E. Every marker
  sits on its own track, and the polar tracks cross the map's top/bottom edges.
- Red check: temporarily reintroducing card 16's raw-axis marker bug in
  `compose_satview_markers()` made both comparisons fail (globe 454 px,
  0.0739%; map 886 px, 0.1442%) with markers visibly off their tracks; the
  composer was restored byte-for-byte and both passed again.
- Blessing: `py do.py blesssatviewplugin debug --skip-build` 13 s and
  `blesssatviewmap` 12 s; both blessed BMPs are byte-identical to the
  offscreen captures inspected above. Two consecutive captures of each scene
  differed in 0 pixels.
- `py do.py test debug --satview --reconfigure` (manifest is read at
  configure time): 214 s wall, 61 CTest entries, 60 passed in 120.78 s.
  SatView: C++ shards 51.38 s / 26.96 s, catalog Python 1.44 s,
  `draxul-render-satview-plugin` 11.22 s, `draxul-render-satview-plugin-map`
  10.47 s. The single failure, `draxul-test-server-shard-0` (personal schedule
  `remove_all` temp cleanup), is the known unrelated core failure recorded in
  card 16. Final rebuild then `ctest -R draxul-render-satview-plugin`: 2/2
  passed, 10.33 s and 10.27 s.
- `py do.py smoke debug --skip-build`: exit 0 in 6 s (final rerun 5 s); do.py
  shut down its own test server, and the user's server was untouched.
- Render-manifest `do.py` unit tests (9 selected) pass.
- Logs and red-check artifacts: `build-ninja-debug/windows-gates/satview-render-fixture/`.

### Remaining limits

- macOS references are not blessed and `satview_render.mm` cannot be compiled
  here. The fixture path is backend-neutral (shared adapter TU, runtime and
  scene records), but adding `"macos"` to both manifest entries needs a Mac
  bless and review; that belongs in a follow-up card rather than this one.
