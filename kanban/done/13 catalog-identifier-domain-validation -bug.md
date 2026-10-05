# Validate satellite identifiers before arithmetic
**Summary:** Reject invalid satellite identifiers before conversion so unusual catalog data cannot cause invalid arithmetic.

**Priority:** 13  
**Severity:** CRITICAL  
**Source:** `plugins/satview/src/core/satview_catalog.cpp`  
**Reported by:** Claude M14; consensus F22.

**Evidence and trigger:** Line 560 admits the rounded upper endpoint; propagation calls `llabs` at `satview_propagation.cpp:264`. A tampered bundled sample can supply `INT64_MIN` because startup fallback bypasses merge validation.

- [x] **Investigate:** Trace exact identifier parsing and validation through network, cache, sample fallback, and direct model compilation.
  - GP JSON (network, startup cache, bundled sample) used `int64_field`, which admitted
    `INT64_MIN`, zero, negatives, fractions (rounded), and the double `2^63` endpoint
    (`double(INT64_MAX)`), then converted with `llround`. Only `merge_satellite_catalogs`
    rejected `<= 0`, and the sample fallback skips the merge. SATCAT CSV already used an
    exact `from_chars` parse and rejected `<= 0`; disposition/ephemeris CSV ids are only
    used as lookup keys. `build_satellite_propagation_model` compiles caller-supplied
    records directly, reaching `std::llabs(INT64_MIN)` in `sgp4_satellite_number`.
- [x] **Fix:** Enforce positive representable identifiers before conversion; avoid admitting the floating upper endpoint.
  - `catalog_id_field` replaces `int64_field`: decimal strings must parse exactly as a
    positive int64; JSON numbers must be integral and in `[1, 2^53)`, where conversion is
    exact. Every GP entry point (network, cache, sample) goes through it.
- [x] **Fix:** Make propagation arithmetic safe for invalid direct inputs independently of parser validation.
  - `sgp4_satellite_number` takes the magnitude in unsigned arithmetic, so every int64
    (including `INT64_MIN`) is well defined.
- [x] **Acceptance:** Minimum signed values, the exclusive upper endpoint, zero, and unsupported identifiers fail safely at every applicable boundary.
  - `SatView GP catalog identifiers are validated before conversion`
    (`tests/satview_catalog_tests.cpp`): INT64_MIN, INT64_MAX/2^63, 1e300, 2^53, 0, -1,
    fractions, non-numeric strings, booleans, and null are skipped as malformed; a
    tampered bundled sample loaded through `load_sample_satellite_catalog(root)` keeps only
    its valid record.
  - `SatView propagation compiles directly supplied out-of-domain identifiers safely`
    (`tests/satview_propagation_tests.cpp`) compiles and propagates INT64_MIN, -1, 0, and
    INT64_MAX directly.
- [x] **Acceptance:** Valid catalog identifiers retain their identity and compile normally.
  - Same tests: 1, 25544, 25544.0, 2^53-1, "900002", and "9223372036854775807" keep their
    exact identity; directly compiled records keep their id and match Vallado case 00005.
- [x] **Validation:** Run the SatView-scoped aggregate and same-cache smoke; use undefined-behavior instrumentation where available.
  - macOS: focused `[catalog-id]`, SatView-scoped aggregate, and same-cache smoke. The build
    tree has no UBSan configuration, so the old and new `sgp4_satellite_number` arithmetic
    and the old `2^63` cast were checked in a standalone `-fsanitize=undefined,float-cast-overflow`
    build: the old code reports "negation of -9223372036854775808 cannot be represented" and
    "9.22337e+18 is outside the range of representable values"; the new arithmetic is clean.
    Windows via CI.
