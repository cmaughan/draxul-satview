# Unify SatView pause state and frame scheduling
**Severity:** HIGH  
**Source:** Codex #17; `plugins/satview/src/satview_plugin.cpp:361`.

Wrapper and runtime pause flags diverge: Space pause followed by panel Resume restarts simulation while continuous presentation remains stopped.

**Investigation**

- [ ] Trace Space, captured keyboard input, panel controls, persistence, and tick/render deadlines.

**Fix strategy**

- [ ] Establish one authoritative pause state and synchronize worker controls, scheduling, and stored state through it.

**Acceptance criteria**

- [ ] Space and panel Pause/Resume combinations keep simulation, presentation, and stored state consistent.
- [ ] Captured Space causes no unintended transition; run SatView aggregate tests and same-cache smoke.
- [ ] Share restore coverage with `plugins/satview/kanban/pending/15 satview-restored-worker-settings -bug.md`.
