# Make catalog cancellation coverage reliable during parallel validation

**Summary:** Give the cancellation test a reliable, isolated catalog seed so unrelated worker setup does not exhaust its fixed wait before cancellation is exercised.

**Priority:** P2
**Severity:** MEDIUM — validation reliability; no observed catalog data loss.
**Source:** `tests/satview_network_transport_tests.cpp:83,175`

## Evidence (2026-10-02)

- Root `py do.py test debug --products` failed `draxul-test-satview-shard-1`
  at line 181: `REQUIRE(wait_for_catalog_idle(seeder))`. Seed `1147121151`;
  shard result 97/98 cases, 7875/7876 assertions, elapsed 24.11 seconds.
- A focused rerun passed all 14 assertions. A serial CTest rerun passed in
  23.60 seconds. The exact shard and original seed also passed serially:
  `draxul-test-satview.exe --shard-count 2 --shard-index 1 --rng-seed 1147121151
  --reporter compact` (98 cases, 7889 assertions).
- The idle helper polls 200 times with 5ms sleeps, giving the seeder roughly
  one second before failure. Its fetch callback returns fixture strings; this
  failure does not require live network access and occurs before cancellation
  assertions. The catalog worker also publishes caches and reads/parses bundled
  lunar assets, including the current 2,395,479-byte ephemeris CSV.
- The affected catalog/service/test sources were unchanged by the core critical
  fixes or SatView texture-owner fix. The parallel-load explanation is an
  inference from the fixed setup deadline and serial passes; precise worker
  timings still need measurement.
- Related: [test ownership and isolation](01%20satview-test-boundaries%20-refactor.md).
  The existing asset-root race card describes a separate concurrency defect.

- [ ] **Investigate:** Measure seeder cache/asset work and scheduling delays in the parallel product aggregate.
- [ ] **Fix:** Isolate setup assets or provide an observable completion deadline suited to setup; preserve the actual cancellation shutdown budget.
- [ ] **Acceptance:** The cancellation assertions run reliably in serial and parallel coverage, including the original seed; a stalled seeder still produces actionable diagnostics.
- [ ] **Validation:** Run the SatView aggregate and same-cache smoke; repeat parallel validation enough to cover the measured contention.
