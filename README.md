<p align="center">
  <img src=".github/LatticeLogo.png" alt="Lattice logo" width="200">
</p>

# Lattice

## Table of Contents

- [Overview](#overview)
- [AI Coding Agent Friendliness](#ai-coding-agent-friendliness)
- [Core Architectural Concepts](#core-architectural-concepts)
  - [Multi-Layer Scale System](#multi-layer-scale-system)
  - [Voxel Modes](#voxel-modes)
  - [Cascading Decomposition](#cascading-decomposition)
  - [Coordinate Precision](#coordinate-precision)
  - [Material-Driven Simulation](#material-driven-simulation)
  - [Macro-Voxel Composition Recipes](#macro-voxel-composition-recipes)
  - [Lazy Decomposition Details](#lazy-decomposition-details)
  - [Voxel Editor Interoperability](#voxel-editor-interoperability)
- [Project Structure](#project-structure)
- [Plugin API](#plugin-api)
- [Setup](#setup)
- [Getting Started](#getting-started)
- [Design Constraints](#design-constraints)
- [Roadmap to 1.0](#roadmap-to-10)
  - [Remaining before 1.0](#remaining-before-10)
  - [Post-1.0 — Deferred Features](#post-10--deferred-features)
- [Further Reading](#further-reading)
- [License](#license)

---

## Overview

Lattice is a plugin-based C++ game engine designed for creating voxel-based games that go well beyond the conventional single-scale Minecraft model. Its defining architectural features are:

- **Hierarchical multi-layer scale system** — game makers define any number of voxel layers, each with its own base unit size, from centimeters to kilometers
- **Three voxel modes** — Composite (lazily decomposes on demand), Immutable (collision/rendering only, no decomposition), and Terminal (player-buildable leaf layer)
- **Cascading lazy decomposition** — macro-voxels decompose one layer at a time through a chain of intermediate composite layers, never jumping scales in a single step
- **Material-driven simulation** — voxels carry physical properties rather than hardcoded block type logic, enabling composable modding and emergent simulation
- **Standard tool interoperability** — compatible with `.vox` (MagicaVoxel) and `.qb` (Qubicle) for single-layer palette-color content, plus `.bbmodel` (Blockbench) import for per-face textured blocks; extended engine-native `.vxe` format for multi-layer and material features
- **Plugin architecture** — nearly all engine behavior including world generation, features, physics, and import/export is extensible via plugins
- **AI agent-friendly design** — explicit invariants, a strong `WorldCoord` type, flat callback-based plugin hooks, and dedicated architecture documentation

The engine is designed to support games ranging from a conventional Minecraft-style single-scale world all the way to a multi-scale planetary simulation where players build meter-scale structures inside hundred-kilometer terrain voxels — or a flying game where the terrain is pure backdrop and only a small playspace layer is interactive at all.

---

## AI Coding Agent Friendliness

This engine is deliberately designed to work well with AI coding agents (Copilot, Claude Code, Cursor, and similar tools). This is not an afterthought — several internal design decisions exist specifically to make agent-assisted development reliable rather than subtly wrong.

Concretely, this means:

- **Invariants are machine-enforced, not just documented.** The `WorldCoord` type makes accidental float promotion a compile error. The `LayerConfig` validator makes invalid layer configurations a startup error. Constraints that only exist in comments get silently violated; constraints enforced by the compiler or runtime do not.
- **The plugin API is flat callback registration, not deep inheritance.** An agent can read `include/plugin_api.h` and understand the complete set of extension points without tracing a class hierarchy. Each plugin's contract is self-contained.
- **Subsystem dependencies are explicit and bounded.** The architecture document defines which subsystems may depend on which others. An agent working on decomposition does not need to understand the renderer. An agent working on the renderer does not need to understand physics.
- **Negative rules are written down explicitly.** The things you must *not* do — introduce float arithmetic in world-space paths, add non-deterministic calls to the decomposition pipeline, skip levels in the decomposition chain — are documented as named rules, not left to be inferred from context.

**If you are an AI coding agent starting work on this codebase, read [`docs/architecture.md`](docs/architecture.md) before writing any code.** It contains the subsystem map, the hard invariants, a list of common mistakes, and a heuristic for when to proceed independently versus when to raise a design question. Working without it will produce code that looks correct but violates load-bearing constraints in ways that are difficult to diagnose.

---

## Core Architectural Concepts

### Multi-Layer Scale System

Rather than fixing a single voxel size, the engine supports a user-defined stack of **layers**, each representing a different scale of the world. A game defines its layer stack in a project configuration file:

```yaml
layers:
  - name: "continental"
    voxel_size_m: 100000.0
    mode: composite
    decompose_to: "regional"

  - name: "regional"
    voxel_size_m: 10000.0
    mode: composite
    decompose_to: "local"

  - name: "local"
    voxel_size_m: 1000.0
    mode: composite
    decompose_to: "terrain"

  - name: "terrain"
    voxel_size_m: 1.0
    mode: terminal

  - name: "detail"
    voxel_size_m: 0.1
    mode: terminal
```

A minimal flying game that uses large voxels only as backdrop:

```yaml
layers:
  - name: "world"
    voxel_size_m: 100000.0
    mode: immutable       # collision + rendering only; no decomposition ever

  - name: "playspace"
    voxel_size_m: 1.0
    mode: terminal        # the only interactive layer
```

A conventional Minecraft-style game defines a single terminal layer and never interacts with any of this complexity.

**Validation rules enforced at engine startup:**

- At least one layer must be defined
- Each layer's voxel size must be strictly smaller than its parent
- The ratio of parent size to child size must be a whole integer (e.g. 10:1 or 100:1 — not 7.3:1)
- Minimum ratio between adjacent layers is 2:1
- Every composite layer must either name a valid `decompose_to` target or have a recipe plugin registered — a composite layer with no recipe is a startup error, not a runtime surprise
- Ratios implying a single-decomposition child grid exceeding the configured voxel budget produce a warning with the actual child count printed explicitly

**Why integer ratios only?** A parent voxel's interior must tile perfectly with child voxels. A 10m parent with 1m children produces exactly 10×10×10 = 1,000 child slots. A 7.3m parent produces a fractional remainder that cannot be represented cleanly in a grid. Non-integer ratios are rejected at load time with a clear error message, not silently rounded.

### Voxel Modes

Every layer operates in one of three modes, declared in the layer config:

| Mode | Decomposition | Player Modification | Persistence | Use Case |
|---|---|---|---|---|
| `composite` | Lazy, on demand | After decomposition | Dirty chunks only | Procedural terrain at any intermediate scale |
| `immutable` | Never | Never | None needed | Backdrop geometry, skybox-as-collision, "floor is lava" terrain |
| `terminal` | N/A (leaf layer) | Yes | Always | Player-buildable space; the finest interactive scale |

Immutable voxels do not participate in upward damage propagation. The propagation chain stops at an immutable boundary.

### Cascading Decomposition

Decomposition is a **chain**, not a single jump. A 100km composite voxel does not decompose directly into 1m child voxels. It decomposes into its declared `decompose_to` layer (e.g. 10km composites), which in turn decompose into their children, and so on, until a terminal layer is reached.

Each step in the chain is independently lazy — intermediate composite layers materialize only when something interacts with them. A player approaching a 100km voxel triggers decomposition to 10km children in the immediate area. Drilling further down triggers decomposition of the relevant 10km children into 1km children, and so on.

Recipes at each composite level can pass **seed parameters and material biases** down to their children, enabling hierarchical constraint: a "mountain range" 10km recipe can constrain what its constituent 1km "peak" recipes generate, without either layer knowing how many levels exist above or below it.

### Coordinate Precision

World-space coordinates are stored and computed using the `WorldCoord` type, which wraps a double-precision 3D vector. Only the final GPU submission path converts to single-precision float, using a **floating origin** — the camera's world position becomes the local origin, and all scene geometry is translated into camera-local space before submission.

This prevents the floating-point precision loss that silently corrupts sub-meter detail at kilometer scales (a 32-bit float has ~7 significant decimal digits; at 100km scale, sub-meter precision is gone entirely).

`WorldCoord` provides explicit named conversion methods rather than implicit casts. This makes accidental float promotion a compile error rather than a silent bug. **Do not use raw `double` or `float` for world-space positions anywhere in the engine or in plugins.**

### Material-Driven Simulation

Voxels do not have a hardcoded "block type ID." Instead, each voxel carries a set of **material properties**:

| Property | Description |
|---|---|
| `density` | Mass per unit volume; affects physics and structural load |
| `structural_strength` | Resistance to collapse under load |
| `thermal_conductivity` | Heat transfer rate; relevant for fire/temperature simulation |
| `porosity` | Fluid permeability |
| `hardness` | Resistance to removal/destruction |
| `palette_index` | Maps to a visual material definition (color, texture, PBR params) |

Mods that add new materials define property values rather than special-case code. Physics, fluid, and voxel-removal systems respond to properties, not IDs. A new volcanic rock mod does not require changes to lava flow logic — it just declares material properties that the existing fluid system already understands.

For voxel-editor compatibility, the `palette_index` field maps to a standard 256-entry palette, allowing `.vox` import/export for the visual material layer even when extended properties are in use.

### Macro-Voxel Composition Recipes

Any composite voxel carries a **composition recipe** describing what it contains at the next layer down. Recipes specify:

- **Material distribution** — a weighted palette of child materials with a noise function controlling spatial arrangement (e.g. "80% granite, 15% quartz veins, 5% iron ore")
- **Feature overlays** — spatially-aware generators that stamp structures into the child grid (cave networks, water tables, ore clusters, dungeon seeds, etc.)
- **Boundary behavior** — how the top, bottom, and sides of the macro-voxel differ from its interior (surface soil, exposed rock faces, ice caps, etc.)
- **Seed parameters** — values passed down to child recipes to constrain their generation

Feature generators are registered by plugins. A recipe references feature generators by name. Cave generation, dungeon placement, and any other sub-structure system are plugin-defined and fully composable.

### Lazy Decomposition Details

When a player interacts with an undecomposed composite voxel:

1. The engine checks whether a decomposed child grid already exists for that voxel
2. If not, the recipe runs on a worker thread to generate the child grid; async pop-in avoids hitching the main thread
3. The interaction proceeds against the now-real child voxels (which may themselves be composites requiring further decomposition)
4. Player-modified child chunks are marked **dirty** and persisted; unmodified recipe-generated chunks can be evicted and regenerated on demand

**Dirty tracking is at chunk granularity within a composite voxel**, not per-voxel, to keep save file sizes tractable. Chunk size is a tunable constant.

**Upward damage propagation:** Material loss in child voxels updates the parent composite voxel's aggregate `density` and `structural_strength`. A heavily hollowed composite voxel can become structurally unsound at its own scale, triggering collapse that may cascade to neighbors. This emergent behavior arises from the material property system without special-case logic. Propagation stops at immutable layer boundaries.

Decomposition is **deterministic** — given the same recipe and world seed, the same child grid is always generated. This is what allows unmodified chunks to be evicted and regenerated transparently.

### Voxel Editor Interoperability

The engine is designed to remain compatible with standard voxel editors for the common case:

| Content type | Format | Compatibility |
|---|---|---|
| Single-layer, palette materials | `.vox`, `.qb` | Full import/export |
| Per-face textured blocks | `.bbmodel` (Blockbench) | Import (one-way) via plugin |
| Multi-layer or anchored content | Engine-native `.vxe` + `.vox` sidecar | Import/export via plugin |
| Extended material properties | `.vxe` | Engine-native only |

A plugin that adds non-standard features should also register an import/export handler. If none is registered, the engine falls back to vanilla `.vox` export (lossy but functional, with a logged warning).

> **Textured-block interop.** Per-face textured voxels are supported via a
> material-keyed texture atlas and a Blockbench (`.bbmodel`) importer plugin. The
> renderer samples per-face UVs from an atlas uploaded at runtime; materials bind
> tiles per face via `set_material_faces`, and the tiling factor is scale-agnostic
> (one authored texture serves 1 m terminal and large composite voxels alike). See
> `docs/m15-textured-voxels-audit.md` for the design audit that preceded the work.

The `.vox` format supports volumes up to 256³ per object. Larger volumes are automatically chunked on import/export. Imported `.vox` content is always assigned to a specific layer and world-space anchor; it has no concept of the other layers.

---

## Project Structure

```
voxel-game-engine
├── src                                    # → voxel-engine library (all sources below)
│   ├── core
│   │   ├── Engine.cpp                     # Engine lifecycle, startup validation (header in include/core)
│   │   ├── EngineConfig.cpp               # Runtime-settable per-frame work budgets
│   │   ├── PluginManager.cpp / .h         # Plugin load/unload, hook registration, ABI guard
│   │   ├── LayerConfig.cpp                # Layer stack definition and validation (header in include/core)
│   │   ├── RecipeValidation.cpp / .h      # Startup validation of recipe/noise/feature references
│   │   ├── Logger.cpp / .h                # Leveled, category-tagged logging (info/debug/warn/error)
│   │   ├── Profiler.cpp                   # Optional CPU zone profiler (header in include/core)
│   │   └── Tuning.h                       # Model constants (compile-time); budget defaults from EngineConfig
│   ├── world
│   │   ├── Voxel.cpp / .h                 # Voxel data: material props, palette index, mode
│   │   ├── Chunk.h                        # Fixed-size voxel grid with dirty tracking
│   │   ├── ChunkCoordMath.h               # Coord conversions: WorldCoord ↔ VoxelCoord ↔ ChunkCoord
│   │   ├── Layer.cpp / .h                 # Per-layer chunk management and coordinate space
│   │   ├── World.cpp / .h                 # Multi-layer world container, interactive-layer selection
│   │   ├── MacroVoxel.h                   # Decomposition state tracking per macro voxel
│   │   ├── DecompositionWorker.cpp / .h   # Async on-demand child grid generation (thread pool)
│   │   ├── DecompositionManager.cpp / .h  # Engine-owned N-layer cascade orchestrator
│   │   ├── LODManager.cpp / .h            # Per-layer streaming budget + eviction (neutral tier)
│   │   ├── StreamingVolume.cpp / .h       # Camera-centered residency: box / sphere / shell
│   │   ├── Recipe.h                       # Owning recipe value type (deep-copies RecipeDesc)
│   │   ├── RecipeResolve.cpp / .h         # Resolve recipe string ids to fns; seed-param cascade
│   │   ├── ResolvedRecipe.cpp / .h        # fillChildChunk: distribution + boundary + features + occupancy
│   │   ├── Noise.cpp / .h                 # Built-in noise: value, fbm, ridged, worley
│   │   ├── VoxelRaycast.cpp / .h          # DDA grid traversal in double precision
│   │   ├── VoxelCollision.cpp / .h        # Swept axis-separated AABB vs terminal voxels
│   │   ├── GravityProvider.h              # gravityAt(WorldCoord) seam: constant / radial / zero-g
│   │   └── AxisRole.h                     # Gravity-relative face role resolution (up/down/lateral)
│   ├── renderer
│   │   ├── RendererFactory.cpp            # createRenderer() impl; sole bgfx-naming public-API code
│   │   ├── BgfxRenderer.cpp / .h          # bgfx backend: window, shaders, floating origin, HUD
│   │   ├── ChunkMesh.cpp / .h             # Per-chunk static mesh build + GPU upload
│   │   ├── ChunkMeshData.cpp / .h         # Face-culled mesh builder with AO, lighting, texture UVs
│   │   ├── Palette.h                      # Runtime 256-entry visual palette (RGBA)
│   │   ├── TextureManager.cpp / .h        # Texture atlas lifecycle: decode → pack → GPU upload
│   │   ├── TextureAtlasData.cpp / .h      # Headless shelf-packer for atlas sub-rects
│   │   ├── MaterialFaces.cpp / .h         # (palette_index, face) → atlas tile + tiling factor
│   │   └── LODManager.cpp / .h            # Backward-compat redirect → src/world/LODManager.h
│   ├── platform
│   │   └── Window.cpp / .h                # GLFW window; exposes native handles
│   ├── simulation
│   │   ├── PhysicsSystem.cpp / .h         # Material-property-driven structural simulation
│   │   ├── PropagationSystem.cpp / .h     # Multi-level upward damage propagation
│   │   ├── RemovalModel.cpp / .h          # Hardness-driven voxel-removal cost function
│   │   ├── RemovalAccumulator.cpp / .h    # Per-target removal progress accumulator
│   │   ├── FluidSystem.cpp / .h           # Gravity-relative fluid flow simulation
│   │   ├── ThermalSystem.cpp / .h         # Thermal conduction simulation
│   │   ├── LightingSystem.cpp / .h        # Sky + block light propagation
│   │   ├── FieldOverlay.h                 # Sparse per-chunk field storage (shared by fluid/thermal/light)
│   │   └── NeighborWalk.h                 # 6-neighbor iteration utilities
│   ├── net
│   │   ├── ITransport.cpp / .h            # Abstract transport seam (swappable via plugin)
│   │   ├── ENetTransport.cpp / .h         # ENet-backed ITransport implementation
│   │   ├── NetworkManager.cpp / .h        # Session management, edit replication, interest mgmt
│   │   ├── NetPackets.h                   # Wire-protocol packet types (little-endian)
│   │   └── NetJoinHandshake.h             # Join sequence: seed + LayerConfig + dirty chunks
│   ├── audio
│   │   ├── IAudioBackend.cpp / .h         # Abstract audio seam (swappable via plugin)
│   │   ├── MiniaudioBackend.cpp / .h      # miniaudio-backed IAudioBackend (real + null device)
│   │   ├── AudioManager.cpp / .h          # Listener, emitters, material→sound resolution
│   │   └── AudioValidation.cpp / .h       # Startup validation of sound/binding registrations
│   ├── io
│   │   ├── VoxImporter.cpp / .h           # .vox format import with layer assignment
│   │   ├── VoxExporter.cpp / .h           # .vox format export with auto-chunking
│   │   ├── QbImporter.cpp / .h            # .qb (Qubicle) import with RLE/BGRA support
│   │   ├── QbExporter.cpp / .h            # .qb format export
│   │   └── ChunkPersistence.cpp / .h      # .vxc chunk save/load codec (versioned)
│   └── plugins
│       └── ExamplePlugin.cpp / .h         # Reference plugin: feature generator + material def
├── demos                                  # Progressive series of reference examples (20 demos)
│   ├── 01-single-voxel/                   # single voxel in space (auto-orbit / free-cam)
│   ├── 02-streaming-terrain/              # chunked terrain flythrough
│   ├── 03-plugin-driven-world/            # disk-loaded plugins, live water toggle
│   ├── 04-build-break-persist/            # place/break voxels, walk, save/load
│   ├── 05-decompose-on-approach/          # multi-layer decomposition + immutable backdrop
│   ├── 06-magicavoxel-round-trip/         # .vox import → edit → export
│   ├── 07-arena-platformer/               # 5-layer platformer, collect-the-keys
│   ├── 08-material-matters/               # hardness-driven mining, HUD readout
│   ├── 09-recipe-built-voxel/             # recipe decomposition, caves, ore, seed params
│   ├── 10-drill-to-the-core/              # 4-layer cascade, cache eviction round-trip
│   ├── 11-shared-world/                   # two-player multiplayer, chat, interest mgmt
│   ├── 12-soundscape/                     # positional audio, material sounds, ambient bed
│   ├── 13-structural-collapse/            # structural load/collapse, crumble response
│   ├── 14-flow-and-heat/                  # fluid seepage, thermal conduction, HUD probe
│   ├── 15-textured-blocks/                # Blockbench import, per-face textured rendering
│   ├── 16-beyond-blocks/                  # zero-g island, shell backdrop, heterogeneous budgets
│   ├── 17-asteroid-belt-miner/            # radial gravity, volumetric asteroids, surface-cam
│   ├── 18-hud-and-controls/               # health/inventory/minimap HUD, gamepad support
│   ├── 19-multilevel-collapse/            # multi-level propagation, grandparent cascade
│   ├── 20-mega-demo/                      # "Overworld" survival slice — seeded terrain,
│   │   └── main.cpp                       #   caves, trees, water, zombies, textured blocks, audio
│   └── 21-voxel-world/                    # six-biome infinite/bounded world, two-speed flight
│       └── main.cpp                       #   (seeded worldgen, streaming, invisible world border)
├── plugins                                # Runtime-loadable plugins, each a MODULE shared lib
│   ├── base-terrain/                      # Materials + terrain layer generator (the streaming-terrain world)
│   ├── water/                             # Removable: water material + sea-level feature generator
│   ├── layered-world/                     # blocks/terrain/backdrop generators
│   ├── recipe-world/                      # composition recipe + cave/ore feature generators
│   ├── server-authority/                  # authoritative-server / host-as-authority P2P
│   ├── chat/                              # in-engine chat over the network message channel
│   ├── material-audio/                    # material-driven break/place sounds
│   ├── crumble/                           # structural-collapse response (clear unstable voxels) [experimental]
│   ├── falling-debris/                    # visual falling-voxel effect on collapse [experimental]
│   ├── flow/                              # fluid field → translucent water voxel responder
│   ├── field-sources/                     # fluid/heat source emitters
│   ├── blockbench/                        # .bbmodel importer (per-face textured blocks)
│   ├── asteroid-field/                    # volumetric radial-density asteroid generator
│   ├── floating-playspace/                # finite floating island generator
│   ├── atmospheric-mist/                  # distance fog with breathing density
│   ├── range-attenuation/                 # flickering torch-radius fog
│   ├── procedural-sky/                    # gradient sky (day/dusk/space + day-night cycle)
│   ├── kinematic-body/                    # reference multi-body kinematic system
│   │   ├── kinematic_body.h               #   Shared API header (body registry, input, state)
│   │   └── plugin.cpp                     #   Body registry, gravity, jump, sweep-and-resolve tick
│   ├── keyboard-mouse/                    # rebindable keyboard/mouse input mapping
│   ├── gamepad/                           # gamepad input with radial dead-zone handling
│   ├── example-hooks/                     # teaching catalog of all major hook types
│   ├── material-showcase/                 # strata world with varied hardness + bedrock
│   ├── arena/                             # arena world generators and materials
│   ├── hazards/                           # lava hazard pools (removable)
│   ├── drill-world/                       # 4-layer cascade generator
│   ├── overworld/                         # seeded rolling terrain + caves + ore
│   ├── voxel-world/                       # six-biome worldgen (climate fields + biome decoration)
│   ├── trees/                             # tree placement feature generator
│   └── mob/                               # wander/chase/attack zombie AI
├── tests                                  # Unit tests; link voxel-engine + GoogleTest (~500 tests)
├── bench                                  # Performance benchmarks (headless, no window)
│   └── profile_pass.cpp                   # CPU profiling harness for decomp/streaming/meshing
├── assets                                 # Runtime assets (audio, Blockbench samples)
│   ├── audio/                             # Material sounds, ambient bed (synthesised on first run)
│   └── blockbench/                        # Sample .bbmodel + generate script
├── shaders                                # bgfx .sc shader sources + committed bytecode
│   ├── vs_voxel.sc / fs_voxel.sc          # Authored shaders (with varying.def.sc)
│   └── generated/                         # Per-backend bytecode headers (committed; see ARCHITECTURE §9)
├── include                                # Public API (propagated to engine consumers)
│   ├── plugin_api.h                       # Public plugin interface; flat callback registration
│   ├── WorldCoord.h                       # Double-precision coordinate type; wraps dvec3
│   ├── core
│   │   ├── Engine.h                       # Engine lifecycle / front-end entry point
│   │   ├── LayerConfig.h                  # Layer stack definition and validation
│   │   ├── EngineConfig.h                 # Runtime per-frame work budgets
│   │   ├── EngineMetrics.h                # Queryable engine stats (frame time, draw calls, etc.)
│   │   └── Profiler.h                     # Optional CPU zone profiler (VOXEL_PROFILE_SCOPE)
│   ├── renderer
│   │   ├── Renderer.h                     # Abstract renderer interface (no bgfx types)
│   │   ├── RendererFactory.h              # createRenderer(): bgfx-free renderer seam
│   │   ├── CameraBasis.h                  # Pitch/yaw/roll + arbitrary up → orthonormal camera frame
│   │   ├── Fog.h                          # FogParams: color, near/far band, density
│   │   ├── Sky.h                          # SkyParams: procedural zenith/horizon/ground gradient
│   │   └── Frustum.h                      # View-frustum culling (bounding-sphere test)
│   └── platform
│       └── NativeWindowHandles.h          # Library-neutral window↔renderer seam
├── templates                              # Copy-paste starting points for a new game (not built)
│   ├── game                               # main.cpp entrypoint + annotated world.yaml
│   └── plugin                             # world-generation and gameplay plugin templates
├── docs
│   ├── architecture.md                    # Subsystem design, invariants, AI agent guidance
│   ├── configuration-guide.md             # Every tunable knob: YAML, Tuning.h, runtime APIs
│   ├── creating-voxels.md                 # Material definitions, recipes, asset import
│   ├── save-format-versioning.md          # .vxc format, version contract, migration path
│   ├── tutorials/                         # Step-by-step walkthroughs (01-14 core, 15-17 advanced)
│   └── proposals/                         # Design proposals (recipe-occupancy, etc.)
├── CMakeLists.txt
├── CONTRIBUTING.md
├── THIRD-PARTY-LICENSES.md
├── LICENSE
└── README.md
```

---

## Plugin API

Plugins register flat callbacks for named engine hooks rather than subclassing engine types. This keeps each plugin's contract visible and self-contained.

Registerable hooks (see `include/plugin_api.h` for the full list and signatures):

- **World generation** — `register_layer_generator`: procedural population per named layer
- **Feature generators** — `register_feature_generator`: parameterized sub-structure stamps used by composition recipes (caves, ore veins, etc.)
- **Composition recipes** — `register_recipe`: material distribution, boundary caps, feature overlays, occupancy carving, and seed parameters for composite-layer decomposition
- **Noise functions** — `register_noise` / `resolve_noise`: pluggable noise (value, fbm, ridged, worley built-in; overridable)
- **Material definitions** — `register_material`: register new materials with property values and palette entries
- **Texture and appearance** — `register_texture` / `register_texture_data` / `set_material_faces` / `set_palette_color`: per-face textured blocks via a runtime atlas
- **Import/Export handlers** — `register_importer` / `register_exporter`: custom format support (`.vox`, `.qb`, `.bbmodel` built-in)
- **Simulation hooks** — `register_on_structural_event`, `register_on_fluid_event`, `register_on_thermal_event`, `register_on_lighting_event`, `register_light_source`, `register_fluid_source`, `register_heat_source`
- **Layer lifecycle hooks** — `register_on_chunk_created` / `register_on_chunk_evicted` / `register_on_voxel_modified`
- **Per-frame tick** — `register_on_tick`: called every frame with `dt`; drives kinematic stepping, animation, or any per-frame plugin simulation
- **Collision primitive** — `move_aabb`: sweep-and-resolve an AABB against terminal voxels; returns resolved position and per-axis hit flags
- **Networking** — `register_on_edit_received`, `register_on_player_joined` / `register_on_player_left`, `register_on_network_message`, `send_network_message`, `apply_edit`, `register_authority_policy`, `register_interest_filter`
- **Audio** — `register_sound`, `register_material_sound`, `play_sound` / `play_material_sound`, `create_emitter` / `set_emitter_position` / `stop_emitter`

To create a plugin, implement the interface defined in `include/plugin_api.h` and load it via `PluginManager`. See `src/plugins/ExamplePlugin` for a worked example covering feature generator registration and material definition.

---

## Setup

The engine builds as a library (`voxel-engine`); demos and games are separate
executables that link it. The build is static by default — pass
`-DBUILD_SHARED_LIBS=ON` to produce a shared engine library instead.

The build is verified on Linux (GCC) — `cmake -B build && cmake --build build`
configures and compiles cleanly from a fresh checkout, and `ctest --test-dir
build` passes. macOS/Clang and Windows/MSVC are likewise supported.

### Prerequisites

You install the toolchain; CMake fetches the libraries. Before your first build
you need:

- A **C++20** compiler — GCC, Clang, or MSVC. (C++20 is a hard floor: `bx`'s SIMD
  headers use designated initializers.)
- **CMake ≥ 3.16**
- **Git** — dependencies are cloned at configure time via `FetchContent`.

Per-OS, a few system packages are also needed (most are preinstalled):

| OS | What to install | Notes |
|---|---|---|
| **Linux** | X11 development headers (optionally Wayland) for GLFW; ALSA and/or PulseAudio dev libs for audio | `libdl`, `libm`, `pthread` are part of the base system. On Debian/Ubuntu: `xorg-dev`, `libasound2-dev` and/or `libpulse-dev`. |
| **macOS** | Xcode command-line tools (provides Clang) | CoreAudio / AudioToolbox / AudioUnit are system frameworks — nothing to install. |
| **Windows** | Visual Studio with the C++ workload (MSVC) | `ws2_32`, `winmm`, `ole32`, `user32` are system libraries — nothing to install. |

### Dependencies fetched automatically

CMake downloads and builds these at configure time (no manual install). All are
pinned for reproducible builds; see [`THIRD-PARTY-LICENSES.md`](THIRD-PARTY-LICENSES.md)
for licenses.

| Dependency | Version | Used for |
|---|---|---|
| bgfx / bimg / bx (via bgfx.cmake) | v1.143.9257-544 | Rendering backend |
| GLM | 1.0.1 | Math types (`WorldCoord`) |
| yaml-cpp | 0.8.0 | Layer-config parsing |
| GLFW | 3.4 | Windowing and input |
| ENet | v1.3.17 | Networking transport |
| miniaudio | 0.11.21 | Audio |
| GoogleTest | pinned to `main` | Tests only — skipped if `external/googletest` is present |

### What to expect from the setup process

- The **first** `cmake -B build` is slow and **requires network access** — it
  clones several repositories plus bgfx's git submodules. Subsequent configures
  reuse the cached sources.
- **No shader toolchain is required.** Shader bytecode is precompiled and
  committed (`shaders/generated/`); only `-DVOXEL_BUILD_SHADERS=ON` pulls in the
  shaderc toolchain, and only when you are regenerating shaders.
- A clean configure + build from scratch is a few minutes (bgfx dominates); the
  engine and demos themselves are quick.

### Troubleshooting

- **Configure fails downloading dependencies** — you are offline or behind a
  proxy. The first configure needs network access to clone the dependencies.
- **CMake policy / "compatibility with CMake < 3.5" error** — your CMake is older
  than 3.16. Upgrade it.
- **Linux: GLFW fails to configure, or X11/`xkb` headers not found** — install the
  X11 development headers (`xorg-dev` on Debian/Ubuntu).
- **Linux: no audio at runtime** — install the ALSA and/or PulseAudio dev libs
  before configuring; miniaudio binds whichever backend it finds at build time.
- **Compiler errors about designated initializers or `<concepts>`** — your
  compiler is not in C++20 mode or is too old; use a C++20-capable toolchain.

### Building and running the demos

The `demos/` directory holds a progressive series of reference examples, each a
standalone target named after its folder. Every `demos/<NN-name>/` with a
`main.cpp` is discovered automatically — `cmake --build build` builds them all,
or build one with `--target <NN-name>`. To add a demo, drop in a new
`demos/<NN-name>/main.cpp`; no CMake edits are needed.

The `plugins/` directory is discovered the same way: every `plugins/<name>/`
with a `plugin.cpp` is built as a runtime-loadable shared library into
`build/plugins/`. The `03-plugin-driven-world` demo loads them from there.

```bash
git clone <repository-url>
cd voxel-game-engine
cmake -B build
cmake --build build

# Run the first demo (single voxel in space).
# Single-config generators (Make/Ninja):     ./build/01-single-voxel
# Multi-config generators (Visual Studio):    ./build/Debug/01-single-voxel.exe

# Run the plugin-driven world: terrain + materials come from disk-loaded
# plugins; press P to load/unload the water plugin and watch it flood/drain.
# Single-config:   ./build/03-plugin-driven-world
# Multi-config:    ./build/Debug/03-plugin-driven-world.exe

# Run build/break/persist: fly down, press G to drop into the walking
# player (gravity + collision), left/right-click to break/place voxels (1-9
# pick the material), then quit (ESC) and relaunch to confirm your edits
# reloaded. Edits are saved to a "voxelsave/" directory beside the working
# directory you launch from.
# Single-config:   ./build/04-build-break-persist
# Multi-config:    ./build/Debug/04-build-break-persist.exe

# Run decompose-on-approach: a three-layer world — composite blocks over a
# terminal terrain child layer, beside an immutable backdrop. Fly toward the
# coarse blocky terrain and watch macro voxels decompose into fine 1 m terrain;
# press G to walk with collision across all layers.
# Single-config:   ./build/05-decompose-on-approach
# Multi-config:    ./build/Debug/05-decompose-on-approach.exe

# Run MagicaVoxel round-trip: a 4×4×4 coloured cube is auto-generated as
# test_model.vox and imported into an "editor" layer; fly around, edit with
# place/remove, then press E to export back to output.vox (auto-chunking and
# lossy-property warning logged to the terminal if applicable).
# Single-config:   ./build/06-magicavoxel-round-trip
# Multi-config:    ./build/Debug/06-magicavoxel-round-trip.exe

# Run the arena platformer: spawn in a five-layer 500 m walled arena.
# Gold key stakes are imported .vox models; walk into each to collect it (G to
# enable walk mode), then reach the goal totem to win. Press P to toggle lava
# hazards on the platforms, E to export your built region to arena-export.vox.
# Single-config:   ./build/07-arena-platformer
# Multi-config:    ./build/Debug/07-arena-platformer.exe

# Run material matters: a flat strata world — soft topsoil over stone, iron,
# and diamond on an indestructible bedrock floor. Aim down and hold left mouse to
# mine; harder materials take visibly longer (the highlight ramps red) and bedrock
# never clears. The HUD reads out the targeted voxel's hardness/density/structural
# strength; right mouse places the selected material (1-6).
# Single-config:   ./build/08-material-matters
# Multi-config:    ./build/Debug/08-material-matters.exe

# Run the recipe-built voxel: a composite ground slab of 8 m blocks over a
# 1 m terminal layer (with an immutable bedrock floor beneath so you never fall
# into the void), decomposed by a composition RECIPE — a granite/basalt
# distribution under a soil cap, carved by a cave-network overlay and threaded
# with ore veins. Fly toward a block (or left-click it) to decompose it; press T
# to toggle the parent "cave_density" seed parameter and watch the world
# re-decompose with visibly different cave density (each value regenerates
# identically on revisit).
# Single-config:   ./build/09-recipe-built-voxel
# Multi-config:    ./build/Debug/09-recipe-built-voxel.exe

# Run the shared world: a two-player session — the host runs the
# authority in-process (host-as-authority P2P), a client joins by address.
# Start each command in its own terminal. Edits replicate through the authority
# within one round-trip; composite blocks decompose locally on each side (watch
# the HUD packet-rate counter stay flat); T opens chat, I cycles the interest
# mode on the host. The session is unauthenticated UDP on port 27777 by default
# (pass a different port after --host / the address): localhost needs no
# firewall change, but allow/forward the port to play across a LAN.
# Single-config:   ./build/11-shared-world --host
#                  ./build/11-shared-world --join localhost
# Multi-config:    ./build/Debug/11-shared-world.exe --host
#                  ./build/Debug/11-shared-world.exe --join localhost

# Run the soundscape: walk (G) and build through the material strata while
# footsteps, breaking, and placing voxels play material-appropriate POSITIONAL
# sounds (chosen from the targeted voxel's palette_index), over a looping ambient
# bed that pans as you move. The material-audio plugin supplies break/place audio
# off on_voxel_modified; the demo fires footsteps from the ground material and
# pushes the listener from the camera each frame. The HUD shows the active voice
# count and the last sound's event/material. The material-audio plugin resolves
# its sound files under assets/audio/ relative to the working directory; if that
# folder isn't in the cwd the demo falls back to the source-tree root, so it plays
# correctly whether launched from the repo root or the build directory. The
# ambient bed is synthesised to ambient_bed.wav in that directory on first run.
# Single-config:   ./build/12-soundscape
# Multi-config:    ./build/Debug/12-soundscape.exe

# Run the flow-and-heat demo — fluid/thermal field simulation: fluid seeps through a porous dam and
# pools against impermeable walls; heat spreads by each material's conductivity.
# Single-config:   ./build/14-flow-and-heat
# Multi-config:    ./build/Debug/14-flow-and-heat.exe

# Run Beyond blocks: a deliberately non-Minecraft, zero-gravity flythrough.
# A single finite floating island (empty above, below, and on every side — a shape
# no heightmap can make) drifts inside a vast, sparse immutable backdrop shell. The
# island streams as a camera-centered BOX volume and the backdrop as a thin SHELL,
# each under its own resident-chunk budget (a heterogeneous-budget case). Fly
# WASD + Space/Shift in any direction — there is no "down" (gravity is zero-g).
# Single-config:   ./build/16-beyond-blocks
# Multi-config:    ./build/Debug/16-beyond-blocks.exe

# Run HUD and controls: a first-person walk/mine/build on the hardness
# strata, the reference for a real game HUD and the three reference plugins.
# Walk with WASD or a gamepad's left stick (the active device auto-switches when
# you grab a controller), hold LMB/RT to mine and bank the material, RMB/LT to
# place it back, 1-5 or the bumpers to pick a slot. The HUD shows a health bar
# (hard landings hurt), an inventory hotbar, a top-down excavation MINIMAP that
# fills in as you dig, and a status line with coords / active device / fps.
# Single-config:   ./build/18-hud-and-controls
# Multi-config:    ./build/Debug/18-hud-and-controls.exe

# Run the MEGA-DEMO ("Overworld"): a Minecraft-lite survival slice tying many
# engine features together — seeded rolling terrain with caves + ore, trees, valley
# water plus a live fluid spring, textured blocks, a wander/chase/attack zombie, and
# a kinematic player with a HUD and ambient nature audio. Pass a world SEED as the
# first argument to choose the world; the SAME seed always regenerates the SAME world
# (terrain, caves, trees, mob spawns), and omitting it uses a deterministic default.
# The active seed is shown on the HUD. (Synthesises its texture/sound assets on first
# run; resolves them from the repo root when launched elsewhere, like 12-soundscape.)
# Single-config:   ./build/20-mega-demo           # deterministic default seed
#                  ./build/20-mega-demo 12345     # choose a world seed
# Multi-config:    ./build/Debug/20-mega-demo.exe 12345

# Run VOXEL WORLD: a Minecraft-style fly-through explorer over six biomes (Ocean,
# Plains, Forest, Desert, Mountains, Snowy Tundra) generated deterministically from a
# world seed. Infinite by default — fly any direction (hold Left-Ctrl for the fast
# traversal speed) and it streams forever with no edge. Pass --size N to cap it to a
# square half-extent of N metres, an invisible "world border" you cannot fly past. The
# HUD reads the seed, infinite/bounded, the biome under the camera, and your position.
# (Synthesises its texture assets on first run, like the mega-demo.)
# Single-config:   ./build/21-voxel-world               # infinite, default seed
#                  ./build/21-voxel-world 12345         # choose a world seed
#                  ./build/21-voxel-world 12345 --size 800   # bounded world border
# Multi-config:    ./build/Debug/21-voxel-world.exe 12345 --size 800

# Run the test suite
ctest --test-dir build
```

Full controls for `04-build-break-persist`: **WASD** move, **mouse** look,
**F** toggles the mouse cursor, **G** toggles walk/fly, **Space/Shift** fly
up/down (or **Space** to jump while walking), **left/right mouse** break/place,
**1**–**9** select the build material, **ESC** quits.

Controls for `05-decompose-on-approach`: **WASD** move, **mouse** look,
**Space/Shift** fly up/down (or **Space** to jump while walking), **G** toggles
walk/fly (collision across all layers), **F** toggles the mouse cursor, **ESC**
quits. Fly toward the coarse blocky terrain to decompose it into fine detail.

Controls for `06-magicavoxel-round-trip`: **WASD** move, **mouse** look,
**Space/Shift** fly up/down, **left/right mouse** break/place, **1**–**9**
select palette material, **E** export the current editor layer to `output.vox`,
**F** toggles the mouse cursor, **ESC** quits.

Controls for `07-arena-platformer`: **WASD** move, **mouse** look, **G** toggles
walk/fly (cross-layer collision + gravity), **Space** jump (walk) or fly up,
**Shift** fly down, **left/right mouse** break/place, **1**–**9** select
material, **P** toggles lava hazards on platforms, **E** exports the detail layer
to `arena-export.vox`, **F** toggles the mouse cursor, **ESC** quits. From the
floor spawn, switch to walk mode and head north to the stone staircase, then jump
up its steps onto the start pad (walk-mode collision has no step-up, so each 1 m
riser is a jump). Walk into gold key stakes to collect them — an on-screen
counter tracks your progress — and reach the goal totem with all four keys to win.

Controls for `08-material-matters`: **WASD** move, **mouse** look, **G** toggles
walk/fly, **Space/Shift** up/down (fly) or jump (walk), **hold left mouse** mine
the targeted voxel (harder materials take longer; bedrock never clears), **right
mouse** place the selected material, **1**–**6** select material, **F** toggles
the mouse cursor, **ESC** quits. Aim straight down and dig through the strata to
feel each material's hardness; the HUD shows the targeted voxel's hardness,
density, and structural strength.

Controls for `09-recipe-built-voxel`: **WASD** move, **mouse** look, **Space/Shift**
up/down (fly) or jump (walk), **G** toggles walk (gravity + cross-layer collision),
**left mouse** decomposes the targeted macro voxel (composite picking), **T** toggles
the parent `cave_density` seed parameter and re-decomposes the world, **F** toggles
the mouse cursor, **ESC** quits. Fly toward the gray block slab (or click a block) to
decompose it into recipe-built terrain — a soil cap over a granite/basalt interior,
carved by caves and threaded with iron-ore veins. Toggle **T** to compare the two
cave densities; revisiting a region regenerates it identically (determinism).

Controls for `11-shared-world`: **WASD** move, **mouse** look, **Space/Shift**
fly up/down (or **Space** jump while walking), **G** toggles walk/fly,
**left/right mouse** break/place, **1**–**9** select the build material, **T**
opens the chat input line (**Enter** sends, **Escape** dismisses), **I** cycles
the interest-management mode on the host (broadcast-all →
mirrored-streaming-radius → plugin distance filter), **F** toggles the mouse
cursor, **ESC** quits. Launch `--host` in one terminal and `--join localhost`
in another; the HUD shows the connected player count, per-peer round-trip time,
the inbound packet rate, the interest mode with its suppressed-edit count, the
shared world seed, and the source of the last replicated edit. Remote players
render as colored marker cubes at their last replicated position.

Controls for `12-soundscape`: **WASD** move, **mouse** look, **G** toggles
walk/fly, **Space/Shift** up/down (fly) or jump (walk), **left mouse** breaks the
targeted voxel (the indestructible bedrock floor never clears), **right mouse**
places the selected material, **1**–**6** select material, **F** toggles the mouse
cursor, **ESC** quits. Switch to walk mode (**G**) and move to hear
material-appropriate footsteps from the voxel under your feet; break and place
blocks to hear their material's positional break/place sounds (supplied by the
`material-audio` plugin off `on_voxel_modified`); move around to hear the ambient
bed pan. The HUD's top line reads out the active voice count and the last sound's
event/material.

Controls for `14-flow-and-heat`: **WASD** move, **mouse** look, **Space/Shift**
fly up/down, **F** toggles the mouse cursor, **1** loads the `flow` responder
(saturated fluid cells realize as translucent water voxels), **0** unloads it
(the fields keep simulating but realize no geometry — field-only), **ESC** quits.
The camera starts in front of the glass tank; watch the blue fluid field fill the
left chamber and seep through the porous sand dam into the right, while the orange
heat field races across the conductive iron floor and barely reaches the rock half.
Aim at a voxel to read its temperature/fluid amount in the HUD probe. (The tank
fills over a few seconds — the fluid model has no sink, so the seep is the
transient; unload/reload `flow` with **0**/**1** to reset and replay it.)

Controls for `16-beyond-blocks`: **WASD** move, **mouse** look, **Space/Shift**
move up/down, **F** toggles the mouse cursor, **ESC** quits. There is no walk
mode and no "down" — the world's gravity policy is zero-g, so flight is pure
6-DOF. Fly around the floating island to watch its camera-centered **box** volume
keep it resident from any side (above, below, or beside — no vertical bias), then
look outward to the sparse immutable **shell** backdrop. The HUD reads the active
gravity policy and each layer's resident-chunk count against its own budget — a
tiny tight playspace and a vast sparse backdrop streaming side by side.

Controls for `18-hud-and-controls`: **keyboard/mouse** — **WASD** move, **mouse**
look, **Space** jump, hold **LMB** mine, **RMB** place, **1**–**5** select an
inventory slot, **F** toggles the mouse cursor, **ESC** quits. **Gamepad** —
**left stick** move, **right stick** look, **A** jump, **RT** mine, **LT** place,
**bumpers** cycle slots. The active device switches automatically to whichever you
last touched, so you can pick up a controller mid-session. Mine the strata to bank
materials in the hotbar (harder layers take longer), place them back from the
selected slot, and watch the top-down minimap fill in the hole you dig; hard
landings drain the health bar, which regenerates while you stand on the ground.

Controls for `20-mega-demo`: **keyboard/mouse** — **WASD** move, **mouse** look,
**Space** jump, **LMB** mine (banks the material), **RMB** place from the selected
slot, **1**–**8** select a material, **F** toggles the mouse cursor, **ESC** quits.
**Gamepad** — **left stick** move, **right stick** look, **A** jump, **RT** mine,
**LT** place, **bumpers** cycle slots (the active device auto-switches to whichever
you last touched). **Choosing a world:** pass a seed as the first launch argument —
`20-mega-demo 12345` (multi-config: `./build/Debug/20-mega-demo.exe 12345`) — and omit
it for a deterministic default; the same seed regenerates the same terrain, caves,
trees, and mob spawns. The HUD status line reads the active **seed**, the selected
material, your coordinates, the live mob count (and how many are hostile), and the
input device. Zombies wander until they sense you, then chase and bite (draining the
health bar); mine into the spring/valley water to watch it flow, and chop trees for wood.

Controls for `21-voxel-world`: **WASD** move, **mouse** look, **Space/Shift** fly
up/down, **hold Left-Ctrl** to fly at the fast traversal speed, **F** toggles the
mouse cursor, **ESC** quits. This is a fly-only explorer (no mining/combat) across
**six biomes** — Ocean, Plains, Forest, Desert, Mountains, and Snowy Tundra —
generated deterministically from the world seed. **Choosing a world:** pass a seed as
the first launch argument — `21-voxel-world 12345` (multi-config:
`./build/Debug/21-voxel-world.exe 12345`) — and omit it for a deterministic default;
the same seed regenerates the same world. By default the world is **infinite**: fly in
any direction (Left-Ctrl to cover ground fast) and it streams forever with no edge.
Pass **`--size N`** to cap it to a square of half-extent `N` metres — a classic
"world border" you cannot fly past (`21-voxel-world 12345 --size 800`). The HUD status
line reads the active **seed**, whether the world is **infinite** or **bounded**, the
**biome** under the camera, your coordinates, and the current flight speed (cruise/FAST).

---

## Getting Started

Once the engine builds (see [Setup](#setup)), there are three on-ramps depending
on what you want to do:

- **Learn the engine concept by concept** — the [tutorial series](docs/tutorials/)
  is a progressive, step-by-step walkthrough. Start with
  [01 — Hello Voxel](docs/tutorials/01-hello-voxel.md) and
  [02 — Your First Plugin](docs/tutorials/02-your-first-plugin.md), then follow the
  numbered sequence (01–14) through materials, recipes, asset import, multi-layer
  worlds, player mechanics, audio, multiplayer, simulation, and performance
  tuning. An **advanced tier (15–17)** then covers building worlds at scale:
  [15 — Large Worlds and Coordinate Space](docs/tutorials/15-large-worlds-and-coordinate-space.md),
  [16 — Wrapping (Toroidal) Worlds](docs/tutorials/16-wrapping-worlds.md), and
  [17 — Seamless Procedural Generation](docs/tutorials/17-seamless-procedural-generation.md).
- **Start a new game from boilerplate** — the [templates](templates/) are
  well-commented, copy-paste starting points: a game entrypoint, an annotated
  layer config, and world-generation / gameplay plugin skeletons. See
  [`templates/README.md`](templates/README.md) for how they fit together.
- **Read working code** — the `demos/` series (each a standalone target, listed in
  [Setup](#setup)) demonstrates every major feature, and the `plugins/` directory
  holds the reference plugins those demos load.
- **Browse the API reference** — the generated
  [Lattice API Reference](https://epicsalvation.github.io/Lattice/api/) covers
  the full public surface: `include/`'s engine and renderer headers, plus each
  reference plugin's extension API.

For materials and composition recipes specifically, see
[`docs/creating-voxels.md`](docs/creating-voxels.md); for the YAML / tuning
surfaces, see [`docs/configuration-guide.md`](docs/configuration-guide.md).

---

## Design Constraints

These constraints are enforced by the engine or its type system. They are documented here so contributors — human or AI — understand *why* they exist and do not work around them.

**`WorldCoord` is mandatory for all world-space positions.** Raw `double` or `float` must not be used for world-space coordinates anywhere in the engine or in plugins. `WorldCoord` wraps a double-precision vector and provides explicit conversion methods. This makes accidental float promotion a compile error. Single-precision floats silently lose sub-meter accuracy at kilometer scales.

**Layer ratios must be integers.** The child grid of a composite voxel must tile exactly. Non-integer ratios are rejected at startup, not silently rounded. Fix the config — do not add a workaround in the decomposition code.

**Every composite layer must have a recipe.** A composite layer with no registered recipe plugin is a startup error. It cannot be left as a runtime gap — there is no valid fallback behavior for an unrecipied composite voxel that a player interacts with.

**Decomposition must be deterministic.** The same recipe + seed must always produce the same child grid. This is what allows unmodified child chunks to be evicted and regenerated transparently. Do not introduce non-deterministic calls (`rand()`, `time()`, unordered container iteration without a stable order) into the decomposition pipeline.

**Dirty tracking is chunk-granular, not voxel-granular.** Per-voxel dirty tracking produces unmanageable save file sizes. The dirty flag marks whole chunks within a composite voxel. Chunk size is a tunable constant — set it conservatively.

**Immutable voxels do not propagate damage upward.** The upward propagation chain stops at an immutable layer boundary. Do not add propagation logic that crosses an immutable layer.

**Decomposition is one layer at a time.** A 100km composite voxel decomposes into its declared child layer (e.g. 10km), not directly into 1m terminal voxels. Each level in the chain is independently lazy. Do not shortcut the chain in the decomposition worker.

---

## Roadmap to 1.0

Lattice's engine-feature milestones (M1 through M18.5) are complete — see
[`docs/milestones.md`](docs/milestones.md) for the full build history, including
the design rationale and *shipped* notes recorded as each one landed. What
remains before the 1.0 tag, and what's deliberately deferred past it, is
tracked below.

### Remaining before 1.0

- [x] Verify docs are all correct — *shipped: full pass over README, ARCHITECTURE, and the tutorial series correcting stale references (project structure, plugin API surface, textured-block interop); the structural-collapse feature's docs (ARCHITECTURE §7, Tutorial 13, `crumble`/`falling-debris` plugin headers) were additionally marked experimental and likely to change, per its known streamed-surface failure mode.*
- [x] Make sure the engine has a name.
- [x] **Demo — Voxel World (Minecraft-style infinite survival):** Another large demo in the spirit of the Mega-Demo, deliberately emulating a classic Minecraft-like world — no mobs/AI this time (that's the Mega-Demo's job). Roughly **six biomes** (one of them ocean) generated deterministically from a user-supplied world seed, with a sane default seed when none is given (the Mega-Demo's seed pattern). Unlike a vanilla Minecraft world, this one is **not bounded** by default: with no size arguments it streams infinitely in all four cardinal (horizontal) directions exactly like the engine's other decomposed worlds, generating forever as the player explores. Optional runtime arguments can instead cap the world to a finite size, so a bounded "classic world border" can be demonstrated too. The player can **fly** at a normal exploration speed and at a much faster traversal speed, so a session can cover the distance of a typical (bounded) Minecraft world in a reasonable amount of time and then keep going to show the world has no edge. — *shipped: `demos/21-voxel-world` on a single terminal `terrain` layer, driven by a new `voxel-world` worldgen plugin that selects six biomes (Ocean, Plains, Forest, Desert, Mountains, Snowy Tundra) from low-frequency climate fields — terrain **height** is a continuous blend of those fields so chunk borders never seam, while biome **identity** is a hard quantization used only for surface material and biome-gated decoration (trees, spruce, cacti). Ocean reuses the `water` plugin's flat sea fill. Infinite by default via `LODManager` streaming; `--size N` caps it to a square half-extent with an invisible clamped border. Two-speed free-cam flight (hold Left-Ctrl to boost). Determinism and full six-biome coverage are covered by `tests/VoxelWorldDeterminismTest.cpp`. AI-agent build metrics (and a cost comparison against the Mega-Demo) are recorded in [`docs/demo21-voxel-world-metrics.md`](docs/demo21-voxel-world-metrics.md).*
- [x] **Demo — "No Man's Voxel" (multi-world flight):** A follow-on riffing on No Man's Sky, built on the Voxel World demo above. The player starts on a "paradise world" — essentially the previous demo's world — but can fly up and out past its local bounds to reach and land on other nearby worlds in the same session, no loading-screen jump. Each additional world is simpler than the paradise world (one or two biomes apiece). Depends on the skybox evaluation (`docs/m17-skybox-evaluation.md`). — *shipped: [`demos/22-no-mans-voxel`](demos/22-no-mans-voxel/main.cpp). The camera flies in one continuous double-precision **scene space**; each world is a distinct engine `World` with its own scene offset (void gaps between footprints), toroidal wrap, seed, and biome/sky/fog profile. At most one world streams at a time (the gaps are "space"); leaving a world means **flying UP** — over an altitude band the sky fades `daySky → spaceSky`, the fog thins out, and above the atmosphere top the torus wrap releases so you drift across the void and descend onto the next world. Four worlds — Paradise (six biomes + ocean), Desert, Snowy, and a dead grey Moon — are all the same `voxel-world` plugin driven per-world by a `VwProfile`, gated so demo 21's world is byte-identical. Number keys 1-4 teleport-and-land on each world. In the space view the sister worlds appear as coarse **proxy planets** you can fly toward (they hand off to real streamed terrain as you arrive), behind a camera-centred **starfield** — both ordinary `ChunkMesh` draws, no new renderer seam. Determinism + moon/water invariants covered by [`tests/NoMansVoxelDeterminismTest.cpp`](tests/NoMansVoxelDeterminismTest.cpp). This is the forcing function the M17 sky/fog seams and `WorldWrap.h` were built for.*
- [x] **API reference documentation:** The tutorials teach the engine by walking through demos, but there is no standalone reference for the plugin API surface (`plugin_api.h` types/functions, hook signatures, `World`/`WorldCoord` interfaces, subsystem `api()` entry points). Generate or hand-write a proper API reference (e.g. Doxygen over the public headers, or a structured Markdown reference under `docs/`) so engine consumers can look up a symbol's contract without reverse-engineering it from a tutorial or demo. — *shipped: Doxygen over `include/` plus each reference plugin's extension-API header (doxygen-awesome-css theme), deployed via GitHub Actions to GitHub Pages at [epicsalvation.github.io/Lattice](https://epicsalvation.github.io/Lattice/), alongside a small landing page linking the API reference, tutorials, demos, and plugins.*
- [ ] 1.0 tag

### Post-1.0 — Deferred Features

Items below were evaluated during the pre-release sanity check (`docs/m17-release-sanity-check.md`) and consciously deferred — not overlooked. Prioritized roughly by expected impact.

*Planned for 1.1*
- [ ] **Dedicated-server mode (F2):** authority without a local player — a headless server that owns the world and serves connected clients, vs. the current host-as-authority-only model. Important for any multiplayer game that needs persistent worlds or higher player counts
- [ ] **Auth / encryption / anti-tamper (F1):** the current networking is explicitly unauthenticated UDP by design. A production multiplayer game will need at minimum session auth and transport encryption

*Simulation & gameplay*
- [ ] **Structural-collapse polish for streamed surfaces (S1):** structural collapse works cleanly on the fixed dioramas (`demos/13-structural-collapse`, `demos/19-multilevel-collapse`) but was **removed from the Mega-Demo** and marked **experimental** — on a large streamed heightmap surface the support-flood misreads ordinary surface mining as an unsupported span and triggers premature cave-ins, and running the detection flood every frame hurt performance. Polishing it for open, streamed worlds (surface-aware support seeding, per-edit rather than every-frame flooding, and re-integration into the Mega-Demo) is deferred here. See `docs/m18-mega-demo-metrics.md` ("A note on scope honesty") and `docs/architecture.md` §7 for the feature's experimental status.

*Rendering*
- [ ] **Time-of-day / directional sun (A3):** sun direction + ambient color as a renderer policy, mirroring the fog policy and §18 gravity policy. Couples to the existing voxel lighting model (A1)
- [ ] **Particle / transient-sprite seam (A4):** a renderer path for short-lived points/quads (break debris, dust, splashes). Today "falling debris" is whole voxels via the `falling-debris` plugin
- [ ] **Texture animation + mipmapping/filtering (A6):** animated atlas frames (flowing lava/water), mip/aniso filtering on the atlas. Currently static, point-sampled
- [ ] **Greedy meshing (A8):** merge coplanar same-material faces into larger quads to reduce vertex count. Standard voxel optimization — evaluate if profiling (`docs/m17-performance-profiling.md`) shows mesh cost dominates
- [ ] **Anti-aliasing / post-processing seam (A9):** MSAA toggle, tonemap hook, and a general post-processing insertion point

*Tooling & developer experience*
- [ ] **Asset hot-reload (D4):** live-reload shaders, textures, and plugin libraries during development without restarting the engine

*Content & assets*
- [ ] **Scripting tier (E2):** Lua/JS/Wren bindings for non-C++ modders. The flat C++ plugin ABI is a deliberate AI-agent-friendly choice for 1.0; a scripting bridge would widen the audience post-release
- [ ] **Unified asset/resource manager (E3):** textures, audio, `.vox`, `.bbmodel` each load ad-hoc per subsystem today. A centralized loader could deduplicate and manage lifetimes — evaluate whether the plugin model makes this unnecessary

*Platform & distribution*
- [ ] **Asset bundling / packaging (G1):** how a shipped game bundles its plugins + assets for distribution. Dependency of the game templates (`templates/`)
- [ ] **Additional rendering backends (G2):** D3D12/DXIL, Wayland-native, WGSL. Already recorded in the architecture gap audit (G9)

*Non-goals (recorded for posterity)*
- **Dynamic rigid-body physics (B2):** tumbling debris, thrown objects beyond axis-resolved AABB — game/plugin concern, not engine-tier
- **Weather / wind fields (B5):** game/plugin policy, not an engine mechanism

---

## Further Reading

The [Lattice API Reference](https://epicsalvation.github.io/Lattice/api/) is a generated, browsable API reference (Doxygen) for the public headers under `include/` and each reference plugin's extension API — use it to look up a type or function's contract once you already know what you're building.

[`docs/architecture.md`](docs/architecture.md) is the primary reference for anyone — human or AI — doing non-trivial work on the engine. It covers:

- The *why* behind every major design decision, not just the what
- A full subsystem dependency map defining which systems may talk to which
- The complete list of hard invariants and common mistakes
- A section written directly at AI coding agents with explicit rules and a proceed-vs-ask heuristic

The README describes what the engine is. ARCHITECTURE.md describes why it is that way and how to work inside it safely.

---

## License

MIT License. See [`LICENSE`](LICENSE) for details.

The engine builds on several third-party libraries, fetched at configure time and
each under its own license; see [`THIRD-PARTY-LICENSES.md`](THIRD-PARTY-LICENSES.md)
for attribution. Contributions are welcome — see [`CONTRIBUTING.md`](CONTRIBUTING.md).
