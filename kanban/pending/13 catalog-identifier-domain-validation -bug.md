# Validate satellite identifiers before arithmetic
**Summary:** Reject invalid satellite identifiers before conversion so unusual catalog data cannot cause invalid arithmetic.

**Priority:** 13  
**Severity:** CRITICAL  
**Source:** `plugins/satview/src/core/satview_catalog.cpp`  
**Reported by:** Claude M14; consensus F22.

**Evidence and trigger:** Line 560 admits the rounded upper endpoint; propagation calls `llabs` at `satview_propagation.cpp:264`. A tampered bundled sample can supply `INT64_MIN` because startup fallback bypasses merge validation.

- [ ] **Investigate:** Trace exact identifier parsing and validation through network, cache, sample fallback, and direct model compilation.
- [ ] **Fix:** Enforce positive representable identifiers before conversion; avoid admitting the floating upper endpoint.
- [ ] **Fix:** Make propagation arithmetic safe for invalid direct inputs independently of parser validation.
- [ ] **Acceptance:** Minimum signed values, the exclusive upper endpoint, zero, and unsupported identifiers fail safely at every applicable boundary.
- [ ] **Acceptance:** Valid catalog identifiers retain their identity and compile normally.
- [ ] **Validation:** Run the SatView-scoped aggregate and same-cache smoke; use undefined-behavior instrumentation where available.
