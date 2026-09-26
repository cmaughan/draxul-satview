# Initialize SatView label text from plugin UI style
**Severity:** HIGH  
**Source:** Codex #15; `plugins/satview/src/runtime/satview_runtime.cpp:2286`.

Production wrappers never provide the host text-service pointer required by label initialization, so sky-label atlases are not created.

**Investigation**

- [x] Trace UI-style font information through label service creation and both backend upload paths. `UiStyleClient` already discovers the host font service; `synchronize_ui_style` feeds `SatViewRuntime::set_imgui_font` at create and tick, and the runtime builds both constellation/cardinal atlas entries.
- [x] Reassess Vulkan label-texture retirement before making uploads reachable. Atlas revision changes now drain prior GPU readers before the shared descriptor is rewritten.

**Fix strategy**

- [x] Initialize the product-owned text service from available font/style metrics and refresh it safely on style changes. The CPU-only integration fixture now drives a fake host UI-style service through the real style client and adapter synchronizer into the runtime, and checks cardinal entries plus generation-triggered atlas rebuilds.

**Acceptance criteria**

- [x] Cardinal and constellation labels render in an actual Windows/Vulkan plugin instance.
- [x] Run SatView aggregate tests and same-cache startup smoke.
- [ ] Verify safe Vulkan and Metal label resource lifetimes under font/atlas refresh; confirm the labels on macOS/Metal.

**2026-09-25 validation:** Product-owned UI-style plumbing already exists. Added Vulkan atlas-retirement synchronization and explicit Metal frame-slot retention for atlas revisions. The fake-host UI-style service -> real client/adapter -> runtime atlas test compiled and passed in the all-products Debug aggregate (49/49 CTest entries). Same-cache Debug startup passed with `py do.py run debug --console -- --smoke-test` (~48 s), and Release startup passed (exit 0). A Windows SatView export rendered the plugin scene but did not prove visible sky labels. The user subsequently confirmed that constellation and cardinal labels both appear in the actual Windows plugin instance. This confirms the Vulkan visual path, not font/atlas refresh lifetime or Metal rendering. The fixed 30 s `smoke --skip-build` timed out on the existing nine-pane Session, so it is not counted as a pass. Resource-lifetime and Metal checks remain before moving to done.

**macOS partial gate (2026-09-26):** The Metal app and SatView plugin built,
the all-products unit inventory passed 47/47 CTest entries, and an exported
native SatView frame rendered the globe, catalog stars, and HDR scene. This
globe scenario does not show cardinal or constellation labels and does not
exercise font/atlas refresh, so the Metal visual and resource-lifetime checkbox
remains open.
