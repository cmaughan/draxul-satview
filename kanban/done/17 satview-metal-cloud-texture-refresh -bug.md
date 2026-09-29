# Refresh Metal cloud textures without mutating in-flight data
**Severity:** HIGH  
**Source:** Codex #18; `plugins/satview/src/render/satview_render.mm:245`.

Same-sized cloud updates overwrite a texture that an earlier asynchronous frame may still sample.

**Investigation**

- [x] Trace cloud revision publication, texture ownership, and outstanding frame usage. Same-sized updates used `replaceRegion` on one shared texture, while frame slots could still sample it.

**Fix strategy**

- [x] Upload into a replacement texture with safe retirement, or synchronize every outstanding reader before mutation. Every revision now gets a new Metal texture, with prior textures retained by their in-flight frame slots.

**Acceptance criteria**

- [x] Repeated same-sized refreshes remain correct with multiple frames in flight.
- [x] Run SatView aggregate tests and same-cache startup smoke; inspect Vulkan parity.
- [x] Run Metal refresh/render checks on macOS, including repeated same-sized updates with multiple frames in flight.
- [x] Keep this scope separate from Vulkan stream-buffer lifetime. The previously referenced card 36 is absent from this board; this change only adds synchronization for cloud and label texture refresh, not stream-buffer handling.

**2026-09-25 validation:** Metal implementation changed, and Vulkan cloud/label uploads now drain existing readers before mutating or rewriting shared descriptors. All-products Debug aggregate passed 49/49 CTest entries; same-cache Debug startup passed with `py do.py run debug --console -- --smoke-test` (~48 s). The fixed 30 s `smoke --skip-build` timed out on the existing nine-pane Session; it is not counted as a pass. macOS Metal build/render validation remains; this card stays pending until then.

**macOS partial gate (2026-09-26):** The Metal app and SatView plugin built,
the all-products unit inventory passed 47/47 CTest entries, and a native
SatView frame export completed with Metal 4x MSAA. The export is a static
frame; it does not prove repeated same-sized cloud refreshes with frames in
flight. Both refresh-specific checkboxes remain open.

**macOS completion (2026-09-29):** The new Metal GPU integration test submits
eight 8x8 cloud revisions and eight 8x8 label-atlas revisions through the
real SatView scene prepass. Frame 0 waits on a shared Metal event while frame
1 uploads and submits, proving that two command buffers are outstanding
across a same-size replacement. The test waits for completion before reusing
slots and checks all buffers completed successfully (20 assertions). Code
inspection confirms each revision creates a new Metal texture and each frame
slot retains the cloud/atlas texture sampled by that frame. No shared texture
is overwritten in place. The real SatView render exports also passed with
Metal 4x MSAA; this card's refresh-specific condition is covered by the GPU
test, not by those static captures. Vulkan parity remains the earlier
reader-drain implementation and does not involve stream-buffer lifetime.

**Validation cost:** Release core + SatView aggregate passed 28/28 entries
(33.76 s), and same-cache Release smoke passed. The GPU case also passed
directly (0.85 s). Debug core + SatView aggregate and startup smoke passed
earlier in this session, before the final shared-event gate was added; the
final gate was validated in Release. No remote Windows test was needed for
this Metal-specific work.

**Hosted CI correction (2026-09-29):** The first hosted macOS run reached the
test but exposed an incorrect test-only assumption that the staged plugin
shader always lived at a fixed bundle path. Plugin staging uses generation
directories, so the test now copies the compiled Metal library into its own
temporary asset fixture and depends directly on the shader compile target.
The corrected GPU case passed locally with 23 assertions. The hosted run also
had unrelated render snapshot and SDK-smoke failures; its full suite did not
pass.
