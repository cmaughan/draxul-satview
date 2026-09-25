# Synchronize restored SatView settings with its worker
**Severity:** HIGH  
**Source:** Codex #16; `plugins/satview/src/satview_plugin.cpp:200`.

Saved preferences are applied after simulation startup without publishing restored time speed or track budgets to the worker.

**Investigation**

- [ ] Compare restored runtime fields against worker startup controls and subsequent synchronization paths.

**Fix strategy**

- [ ] Restore before worker startup or explicitly synchronize every affected setting after applying configuration.

**Acceptance criteria**

- [ ] Nondefault restored speed and track budgets affect worker output before any corrective user action.
- [ ] Cover restore alongside pause-state transitions; run SatView aggregate tests and same-cache smoke.
- [ ] Coordinate with `plugins/satview/kanban/pending/16 satview-authoritative-pause-state -bug.md`.
