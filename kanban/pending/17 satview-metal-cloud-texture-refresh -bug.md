# Refresh Metal cloud textures without mutating in-flight data
**Severity:** HIGH  
**Source:** Codex #18; `plugins/satview/src/render/satview_render.mm:245`.

Same-sized cloud updates overwrite a texture that an earlier asynchronous frame may still sample.

**Investigation**

- [x] Trace cloud revision publication, texture ownership, and outstanding frame usage. Same-sized updates used `replaceRegion` on one shared texture, while frame slots could still sample it.

**Fix strategy**

- [x] Upload into a replacement texture with safe retirement, or synchronize every outstanding reader before mutation. Every revision now gets a new Metal texture, with prior textures retained by their in-flight frame slots.

**Acceptance criteria**

- [ ] Repeated same-sized refreshes remain correct with multiple frames in flight.
- [x] Run SatView aggregate tests and same-cache startup smoke; inspect Vulkan parity.
- [ ] Run Metal refresh/render checks on macOS, including repeated same-sized updates with multiple frames in flight.
- [x] Keep this scope separate from Vulkan stream-buffer lifetime. The previously referenced card 36 is absent from this board; this change only adds synchronization for cloud and label texture refresh, not stream-buffer handling.

**2026-09-25 validation:** Metal implementation changed, and Vulkan cloud/label uploads now drain existing readers before mutating or rewriting shared descriptors. All-products Debug aggregate passed 49/49 CTest entries; same-cache Debug startup passed with `py do.py run debug --console -- --smoke-test` (~48 s). The fixed 30 s `smoke --skip-build` timed out on the existing nine-pane Session; it is not counted as a pass. macOS Metal build/render validation remains; this card stays pending until then.

**macOS partial gate (2026-09-26):** The Metal app and SatView plugin built,
the all-products unit inventory passed 47/47 CTest entries, and a native
SatView frame export completed with Metal 4x MSAA. The export is a static
frame; it does not prove repeated same-sized cloud refreshes with frames in
flight. Both refresh-specific checkboxes remain open.
