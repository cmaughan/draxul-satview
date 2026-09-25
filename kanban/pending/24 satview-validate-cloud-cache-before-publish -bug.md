# Preserve the last valid cloud cache after invalid downloads
**Severity:** MEDIUM  
**Source:** Codex #25; `plugins/satview/src/services/satview_cloud_service.cpp:287`.

Nonempty downloaded bytes replace the cache before decoding, destroying fallback data and potentially suppressing retries after restart.

**Investigation**

- [ ] Trace download validation, cache publication, fallback, and freshness decisions.

**Fix strategy**

- [ ] Decode and validate before atomic publication; retain the prior cache on failure.
- [ ] Retry invalid fresh caches instead of treating freshness alone as sufficient.

**Acceptance criteria**

- [ ] Invalid downloads preserve a usable prior cache, including across restart.
- [ ] Run SatView aggregate tests and same-cache smoke.
- [ ] Coordinate publication changes with existing `kanban/pending/49 satview-concurrent-cache-publication -bug.md`.
