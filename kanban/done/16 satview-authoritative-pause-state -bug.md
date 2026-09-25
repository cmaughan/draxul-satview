# Unify SatView pause state and frame scheduling
**Severity:** HIGH  
**Source:** Codex #17; `plugins/satview/src/satview_plugin.cpp:361`.

Wrapper and runtime pause flags diverge: Space pause followed by panel Resume restarts simulation while continuous presentation remains stopped.

**Investigation**

- [x] Trace Space, captured keyboard input, panel controls, persistence, and tick/render deadlines. The wrapper's duplicate pause flag was not updated by panel controls and toggled captured Space.

**Fix strategy**

- [x] Establish one authoritative pause state and synchronize worker controls, scheduling, and stored state through it. Runtime pause is authoritative; the wrapper observes transitions after input, actions, and panel draw, and persists runtime state.

**Acceptance criteria**

- [x] Space and panel Pause/Resume combinations keep simulation, presentation, and stored state consistent. The real View-panel click test checks runtime pause, tick/presentation notification, and callback state in both directions; the real module integration checks JSON pause save/restore through close/reopen.
- [x] Captured Space causes no unintended transition in the offline runtime test; SatView aggregate tests and same-cache smoke remain in the validation gate below.
- [x] Share restore coverage with `plugins/satview/kanban/done/15 satview-restored-worker-settings -bug.md`.
- [x] Run SatView aggregate tests and same-cache startup smoke.

**2026-09-25 validation:** Offline runtime coverage combines restored settings, action pause/resume, and captured versus uncaptured Space. A focused integration test clicks the actual ImGui View Pause and Resume button (after focusing its dock tab), checking runtime pause, callback-delivered stored state, tick requests, and presentation notifications; it passed three consecutive seeds with 12 assertions each (4.45 s test time). The root real SatView module integration checks JSON pause save/restore across close/reopen through the production storage service. Final SatView-scoped Debug aggregate passed 28/28 CTest entries, including both new paths. Same-cache Debug startup and Release build/startup after the final source edits both exited 0. The fixed 30 s `smoke --skip-build` timed out on the existing nine-pane Session; it is not counted as a pass.
