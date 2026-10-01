# Bound catalog parsing depth
**Summary:** Reject excessively nested catalog data so a malformed download or saved file cannot exhaust the application’s stack.

**Priority:** 12  
**Severity:** CRITICAL  
**Source:** `plugins/satview/src/core/satview_catalog.cpp`  
**Reported by:** Claude M13; consensus F21.

**Evidence and trigger:** Lines 274, 296, and 314 recursively skip object/array values without a depth cap. Deep nesting inside an object field reaches this path before record validation.

- [ ] **Investigate:** Trace network, startup-cache, and bundled-file parser entry points and failure publication.
- [ ] **Fix:** Enforce a bounded nesting depth for both arrays and objects and return useful diagnostics.
- [ ] **Acceptance:** Excessive nesting fails safely without stack exhaustion; permitted boundary depth parses normally.
- [ ] **Acceptance:** Failed refresh retains usable catalog state and startup failure follows the intended fallback.
- [ ] **Validation:** Run the SatView-scoped aggregate and same-cache smoke.
