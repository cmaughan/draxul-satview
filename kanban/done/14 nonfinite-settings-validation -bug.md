# Reject non-finite satellite settings
**Summary:** Reject invalid numeric settings so the satellite view keeps usable lighting and ground coordinates.

**Priority:** P2  
**Source:** `plugins/satview/src/runtime/satview_config_io.cpp`  
**Reported by:** Claude M15; consensus F51.

**Evidence and trigger:** NaN survives the clamp at line 55; infinite longitude becomes NaN at line 235. Lighting and ground calculations receive these invalid values.

**Related:** `plugins/satview/kanban/pending/02 satview-configuration-ownership -refactor.md`.

- [x] **Investigate:** Trace saved/launch configuration and direct runtime application into lighting, projection, and simulation controls.
- [x] **Fix:** Reject non-finite values before clamping or wrapping and retain valid defaults or previous state.
- [x] **Acceptance:** NaN and infinities cannot reach affected rendering/ground calculations through configuration.
- [x] **Acceptance:** Finite boundary settings retain documented clamping and wrapping behavior.
- [x] **Validation:** Run the SatView-scoped aggregate, affected view checks, and same-cache smoke.

## Implementation notes (2026-10-10)

- Saved settings and launch TOML share `apply_satview_table`; its double-number accessor now discards NaN and both infinities before any clamp/remainder. All 14 floating settings are covered (including epoch-age filtering and simulation speed).
- Direct `SatViewRuntime::apply_config` retains the current finite value for each invalid field, then preserves normal finite clamping. Direct longitude/latitude, time speed, and epoch age now receive the same wrap/clamp policy as parsed settings.
- Added saved/launch NaN and ±infinity regression coverage and direct-runtime preservation/boundary coverage. Existing finite persisted boundary tests remain in place.
- Awaiting root-owned core + SatView + ScoreView aggregate and same-cache smoke; acceptance boxes remain open until results are recorded. Both platforms use this backend-neutral runtime/config path.

## Final Debug validation (2026-10-10)

Core + SatView + ScoreView aggregate passed all 62 entries in 45.57s, including
both SatView shards and saved/launch/direct-runtime numeric regressions. The
affected lighting, projection, simulation and filtering inputs retain the exact
previous finite configuration on rejected NaN/±infinity; finite clamp/wrap cases
pass at the real runtime boundary. Same-cache native Metal startup smoke passed.
Both renderers consume this shared finite configuration; Windows follows normal
CI. No product golden was re-blessed. Final Release startup passed (see completed validation below).

## Completed validation

Final aggregate: 62/62 passed (45.57s). Two earlier 62-entry passes took
53.08s and 52.01s and exposed the documented client test synchronization races;
the second also observed a 262ms/250ms checkpoint timing failure that passed
both the first and final aggregate. Focused concurrency diagnosis: 5 epoch
repeats plus 15 paired epoch/pre-wait repeats, all passed after corrections
(20.44s total tests). These repeats overlap aggregate coverage intentionally
while diagnosing failures.

Debug startup smoke passed (~1.2s); basic native Metal comparison passed
(~1.8s) and registered `draxul-render-basic-view` CTest passed (1.66s).
The comparison was repeated once to confirm the registered CTest gate.
Final Release build/configure passed (63.95s total: configure/generate 10.3s,
compile/link ~53.6s); Release isolated startup smoke passed (~0.7s). The first
custom Release runtime exceeded the Unix socket path limit; the standard short
isolated runner succeeded, and no failed helper remains. Debug's initial
aggregate compile took 271.07s after the existing Release cache was reconfigured;
warm aggregate builds and the explicit app link reused that cache. One initial
sandbox build was blocked by compiler-cache access before tests; the authorized
run used the existing compiler cache. Windows/remote CI was not run locally.

Detailed session logs: `/tmp/draxul-easy-cards-aggregate-pass.log`,
`/tmp/draxul-easy-cards-render-ctest.log`, and
`/tmp/draxul-easy-cards-release-smoke.log`.
