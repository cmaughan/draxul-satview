# Initialize SatView label text from plugin UI style
**Severity:** HIGH  
**Source:** Codex #15; `plugins/satview/src/runtime/satview_runtime.cpp:2286`.

Production wrappers never provide the host text-service pointer required by label initialization, so sky-label atlases are not created.

**Investigation**

- [ ] Trace UI-style font information through label service creation and both backend upload paths.
- [ ] Reassess Vulkan label-texture retirement before making uploads reachable.

**Fix strategy**

- [ ] Initialize the product-owned text service from available font/style metrics and refresh it safely on style changes.

**Acceptance criteria**

- [ ] Cardinal and constellation labels render in actual plugin instances.
- [ ] Run SatView aggregate tests, label render checks, and same-cache smoke; verify safe Vulkan and Metal resource lifetimes.
