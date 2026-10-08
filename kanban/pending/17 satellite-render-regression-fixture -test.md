# Make the SatView render regression exercise real satellite placement

**Summary:** Replace the obsolete stylized reference with a controlled real SatView scene so render checks can catch satellite coordinate regressions.

**Priority:** P1 — the existing visual gate neither matches the shipped scene nor reliably exercises satellite markers.

- [x] Record the failed existing golden comparison and inspect the actual/reference images.
- [ ] Provide test-only fixed time, camera and offline catalog/state injection through the real plugin rendering path, without adding user-facing fixture configuration.
- [ ] Exercise known satellite positions and matching tracks in globe and map views, including geographic poles and the equator; require markers to be present.
- [ ] Establish reviewed Windows references using the repository blessing workflow and retain Metal-compatible scene contracts.
- [ ] Run render scenarios, SatView aggregate and same-cache smoke with retained costs/results.

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
