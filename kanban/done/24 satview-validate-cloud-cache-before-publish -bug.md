# Preserve the last valid cloud cache after invalid downloads
**Severity:** MEDIUM  
**Source:** Codex #25; `plugins/satview/src/services/satview_cloud_service.cpp:287`.

Nonempty downloaded bytes replace the cache before decoding, destroying fallback data and potentially suppressing retries after restart.

**Investigation**

- [x] Trace download validation, cache publication, fallback, and freshness decisions. The old worker published bytes before decode and treated a fresh invalid cache as sufficient reason not to fetch.

**Fix strategy**

- [x] Decode and validate before atomic publication; retain the prior cache on failure. Publication now uses unique temporary names and an atomic replacement on Windows and POSIX.
- [x] Retry invalid fresh caches instead of treating freshness alone as sufficient.

**Acceptance criteria**

- [x] Invalid downloads preserve a usable prior cache, including across restart. The cloud service test covers same-process fallback and restart reuse; invalid fresh cache retry is covered separately.
- [x] Run SatView aggregate tests and same-cache smoke. The all-products Debug aggregate and same-cache Debug startup with the explicit `--smoke-test` option passed.
- [x] Confirm final Release startup as required by the repository bug-fix gate.
- [x] Verify concurrent publication scope: the previously referenced card 49 is absent from this board, and this change gives each process/write a distinct temporary filename before atomic replacement.

**2026-09-25 validation:** Added invalid-download/restart and invalid-fresh-cache tests. Focused SatView cloud/host selection passed (15 cases, 127 assertions, 12.33s); all-products Debug aggregate passed 49/49 CTest entries, including SatView. Same-cache Debug startup passed with `py do.py run debug --console -- --smoke-test` (~48 s). Release build/startup passed with `py do.py run release --console -- --smoke-test` (exit 0), rebuilding changed product plugins. The fixed 30 s `smoke --skip-build` timed out on the existing nine-pane Session; it is not counted as a pass. The referenced card 49 is no longer present in this board; unique temp paths avoid same-process publication collisions. No backend-specific rendering changed in this card.
