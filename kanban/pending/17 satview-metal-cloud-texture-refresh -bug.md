# Refresh Metal cloud textures without mutating in-flight data
**Severity:** HIGH  
**Source:** Codex #18; `plugins/satview/src/render/satview_render.mm:245`.

Same-sized cloud updates overwrite a texture that an earlier asynchronous frame may still sample.

**Investigation**

- [ ] Trace cloud revision publication, texture ownership, and outstanding frame usage.

**Fix strategy**

- [ ] Upload into a replacement texture with safe retirement, or synchronize every outstanding reader before mutation.

**Acceptance criteria**

- [ ] Repeated same-sized refreshes remain correct with multiple frames in flight.
- [ ] Run SatView aggregate tests, Metal refresh/render checks, and same-cache smoke; inspect Vulkan parity.
- [ ] Keep this scope separate from `kanban/pending/36 satview-vulkan-stream-buffer-lifetime -bug.md`.
