# SatView Agent Guide

Read the repository-root `CLAUDE.md` before changing this product.

## Ownership boundaries

- `draxul-satview-core` owns catalog formats, the shared CSV tokenizer, domain
  validation, propagation math, projections, and device-free value types.
- `draxul-satview-scene` owns backend-neutral scene composition. Immutable
  composer requests produce generation-tagged scene records; they do not own
  workers, services, ImGui, SDL, or GPU resources.
- `draxul-satview-services` owns catalog/cloud worker lifetime and cache
  publication. A worker remains in flight until the main thread joins it and
  consumes its result.
- `draxul-satview-runtime` owns main-thread publication, service and simulation
  lifetime, view/selection orchestration, panels, and frame scheduling.
- Vulkan and Metal renderers consume the same scene records. Mutable stream
  buffers are frame-slot-owned and may only be rewritten after that slot has
  returned to the renderer.

Keep lexical CSV errors typed in the tokenizer and translate them into each
catalogue's established diagnostics at the caller boundary. Keep numeric and
record policy in the owning catalogue parser.

## Validation

From the Draxul root, run `python3 do.py test debug --satview` followed by
`python3 do.py smoke debug --skip-build`. The product C++ suite is sharded and
all SatView tests carry the `satview` label.
