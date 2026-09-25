# Synchronize restored SatView settings with its worker
**Severity:** HIGH  
**Source:** Codex #16; `plugins/satview/src/satview_plugin.cpp:200`.

Saved preferences are applied after simulation startup without publishing restored time speed or track budgets to the worker.

**Investigation**

- [x] Compare restored runtime fields against worker startup controls and subsequent synchronization paths. The wrapper applies saved TOML after `initialize()` starts the worker, while `apply_config()` previously only changed runtime fields.

**Fix strategy**

- [x] Restore before worker startup or explicitly synchronize every affected setting after applying configuration. Post-start `apply_config()` now updates worker clock controls and render budgets immediately.

**Acceptance criteria**

- [x] Nondefault restored speed and track budgets affect worker output before any corrective user action. Offline host-to-worker test observes speed `120x` and 24 track samples after `apply_config()`.
- [x] Cover restore alongside pause-state transitions; run SatView aggregate tests and same-cache smoke. The shared offline test and all-products Debug aggregate passed; same-cache Debug startup completed with the explicit `--smoke-test` option.
- [x] Confirm final Release startup as required by the repository bug-fix gate.
- [x] Coordinate with `plugins/satview/kanban/done/16 satview-authoritative-pause-state -bug.md`; both share the host-to-worker pause/restore test.

**2026-09-25 validation:** Added offline runtime/worker test for nondefault restored time speed and track sample count. Pause-state integration is implemented alongside this change. Focused SatView cloud/host selection passed (15 cases, 127 assertions, 12.33s); all-products Debug aggregate passed 49/49 CTest entries, including SatView. Same-cache Debug startup passed with `py do.py run debug --console -- --smoke-test` (~48 s). Release build/startup passed with `py do.py run release --console -- --smoke-test` (exit 0), rebuilding changed product plugins. The fixed 30 s `smoke --skip-build` timed out on the existing nine-pane Session; it is not counted as a pass. No backend-specific rendering changed in this card.
