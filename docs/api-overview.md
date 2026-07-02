# Lattice API Reference

This reference is generated from the engine's public headers via Doxygen. It
covers two surfaces:

- **The engine's public API** — everything under `include/`: `plugin_api.h`
  (the native-plugin ABI), `core/Engine.h` and its configuration/metrics
  types, the `renderer/` interfaces, and `WorldCoord.h`. This is what an
  out-of-tree game links against.
- **Reference plugin extension APIs** — the shared `<name>.h` headers a
  handful of in-tree plugins expose alongside the engine ABI (e.g.
  `kinematic-body`, `mob`, `procedural-sky`) so a host application can call
  into them directly instead of only through hook registration.

If you are new to the engine, the
[tutorial series](https://github.com/EpicSalvation/Lattice/tree/main/docs/tutorials)
walks through building a demo end to end; this reference is for looking up a
type or function's contract once you already know what you're building.

## Start here

- @ref PluginContext — the function-pointer table every native plugin
  receives at load time. This *is* the plugin ABI: recipe/material/noise
  registration, voxel-modified and structural-event hooks, networking,
  audio, textured rendering, and the per-frame tick + collision primitive.
- @ref Engine — the top-level engine object: game loop, import/export,
  and the accessors a host uses to attach optional subsystems (networking,
  audio, fluid/thermal/lighting fields, renderer, decomposition manager).
- @ref Renderer — the abstract rendering interface, paired with
  `createRenderer()` (renderer/RendererFactory.h) as the factory seam that
  keeps the concrete bgfx backend out of the public ABI.
- @ref WorldCoord — the double-precision world-space position type used
  everywhere in engine code and plugins.

## Scope

This reference does not cover `src/` — those headers are private to the
engine's own build (see `CMakeLists.txt`'s `PUBLIC include` / `PRIVATE src`
split) and are not part of the supported API surface.

For the engine's design rationale, subsystem dependency map, and invariants,
see [`docs/architecture.md`](https://github.com/EpicSalvation/Lattice/blob/main/docs/architecture.md).
