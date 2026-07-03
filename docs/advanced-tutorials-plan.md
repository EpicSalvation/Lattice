# Advanced Tutorials — Plan

This document is the content plan for an **advanced** tier of the tutorial
series, continuing the numbering of the M18 series (`docs/m18-tutorial-series-plan.md`,
tutorials 01–14). Each tutorial is a standalone Markdown file under
`docs/tutorials/`, usable independently but assuming the reader is comfortable
with the 01–14 material (a plugin, a layer config, the streaming loop).

Where 01–14 teach the engine's **feature surface** — "here is an API, here is
how to call it" — the advanced tier teaches the **concerns you meet when you
build a serious world**: problems the engine deliberately does *not* solve for
you, and the patterns that solve them at the host/plugin level. Every advanced
tutorial follows the same three-beat shape:

1. **Name the concern** — the failure you would hit and why it is real.
2. **The engine's stance** — what the engine does, what it deliberately leaves
   to you, and the reasoning (usually: it cannot be done generically without
   knowing your content, so it is your call).
3. **The pattern** — the concrete technique, anchored to runnable code.

> **Grounding note.** Tutorials 15 and 16 are write-ups of capabilities that
> already ship in `demos/21-voxel-world` (the `--size`, `--wrap`, and teleport
> features and the reusable `demos/common/WorldWrap.h`). They document a
> deliberate architectural choice — world *topology* lives in the host/demo,
> not the engine core — so they should reinforce, not contradict, the
> floating-origin rule in `docs/architecture.md` §1. Cross-link
> `docs/configuration-guide.md` for the flags; do not duplicate large code
> blocks verbatim.

---

## Tutorial index

| #  | File | Title |
|----|------|-------|
| 15 | `15-large-worlds-and-coordinate-space.md` | Large Worlds and Coordinate Space |
| 16 | `16-wrapping-worlds.md` | Wrapping (Toroidal) Worlds |
| 17 | `17-seamless-procedural-generation.md` | Seamless Procedural Generation |

These three form one arc — **"Big, Believable Worlds"** — and read best in
order: 15 establishes how far a world can honestly go and how coordinates stay
precise, 16 makes a finite world edgeless, 17 makes the terrain across any
boundary (a wrap seam, a chunk edge, a decomposition boundary) match.

---

## Per-tutorial outlines

### 15 — Large Worlds and Coordinate Space

**Audience:** Developer building a world large enough that precision and
coordinate limits stop being hypothetical.

**Covers:**

- **The concern.** A 32-bit `float` has ~7 significant digits; at 100 km, a
  world position quantises to metres and the camera visibly jitters. And an
  "infinite" world is never truly infinite — something overflows first.
- **The engine's stance — floating origin (`docs/architecture.md` §1).** All
  world-space positions are `WorldCoord` (a `glm::dvec3`, double precision).
  The *only* narrowing to `float` is `WorldCoord::toLocalFloat(cameraOrigin)`,
  which subtracts the camera first so the GPU only ever sees small, precise
  camera-relative numbers. Why the type exists early: retrofitting precision
  after the fact means auditing the whole codebase.
- **Where the real ceiling is.** Walk the numbers honestly:
  - `double` position: ULP ≈ 7 nm at the 30,000,000 m Minecraft border; does
    not reach even 1 mm until ~4×10¹² m. Not the limit anywhere a player goes.
  - `ChunkCoord` is `int32` (`src/world/Chunk.h`): with 32 m chunks the chunk
    index overflows around 6.9×10¹⁰ m — signed overflow (UB), lookups wrap.
    **This is the true first wall.**
  - `VoxelCoord` is `int64` (`src/world/ChunkCoordMath.h`) precisely so the
    global voxel index does not overflow within the double world extent.
- **The pattern.** How `demos/21-voxel-world` handles it: `kEngineLimitM`
  clamps horizontal position safely short of the int32 wall (1000× the
  Minecraft border); teleport presets jump to far coordinates so you can
  inspect precision at range without flying; `--size` demonstrates a hard
  border in chunk coordinates.
- **How to verify.** Run `21-voxel-world`, teleport to the Minecraft border
  (key 5) and the engine edge (key 6), watch the HUD `xyz` and confirm motion
  stays smooth — precision is intact where a real game would ever reach.

**Outcome:** Reader can reason about how large their world can honestly be,
knows which type fails first and at what distance, and can keep the camera
precise at range.

**Key references:** `include/WorldCoord.h` (`toLocalFloat`),
`src/world/Chunk.h` (`ChunkCoord`), `src/world/ChunkCoordMath.h` (`VoxelCoord`,
`worldToChunk`), `demos/21-voxel-world/main.cpp` (`kEngineLimitM`, teleport
presets), `docs/architecture.md` §1.

---

### 16 — Wrapping (Toroidal) Worlds

**Audience:** Developer who wants a finite world that feels edgeless — a
local planet you can circumnavigate.

**Covers:**

- **The concern.** A finite world has an edge; players fall off it or hit an
  invisible wall. A planet has no edge — cross one side, arrive at the other.
- **The engine's stance — topology is the host's job.** The engine's chunk
  grid is a plain unbounded `int32` lattice; it has no notion of a world that
  wraps, exactly as it has no notion of the `--size` hard border. Wrapping
  therefore lives in the host/demo, and this is deliberate: the coordinate wrap
  is about the *grid*, not the *content*.
- **The two-party contract.** Wrapping splits into two concerns neither party
  can do for the other:
  - **Topology** (host): fold the camera position into the canonical domain
    `[-half, +half)`; fold chunk coordinates onto the torus so both sides of a
    seam reuse *byte-for-byte identical* chunk data; draw each chunk at the
    periodic image nearest the camera. All of this is `demos/common/WorldWrap.h`
    (`wrapCoordM`, `wrapChunk`, `nearestOriginM`) plus the demo's streaming and
    render loop.
  - **Seam continuity** (generator): only the worldgen can make its terrain
    *match* where the edges meet — see tutorial 17.
- **The constraints and why.** Period is a whole, *even* number of chunks
  (so the two seam sides are the same chunks, domain centred on the origin);
  snapped up to at least the streaming diameter (`2·view+2`) so no chunk ever
  needs to appear on both sides of the camera at once.
- **The pattern.** How the demo wires it: `--wrap N`, `voxelworld_set_wrap`
  handed the period + seam-band width, wrap-aware desired/evict sets, and the
  render offset. Honest caveats: caves (3D noise) are not seam-blended;
  features straddling the exact seam can clip like any chunk edge.
- **How to verify.** Run `21-voxel-world --wrap 400`, fly across a seam: the
  HUD `xyz` jumps from `+half` to `-half` (longitude wrapping) while the
  terrain flows continuously — no cliff, no void.

**Outcome:** Reader can turn a finite world into a seamless torus, and
understands why the coordinate wrap and the seam blend are separate jobs.

**Key references:** `demos/common/WorldWrap.h`, `demos/21-voxel-world/main.cpp`
(`--wrap`, streaming fold, render offset), `plugins/voxel-world/plugin.cpp`
(`voxelworld_set_wrap`).

---

### 17 — Seamless Procedural Generation

**Audience:** Developer writing worldgen that must not show boundaries — chunk
edges, decomposition boundaries, or a wrap seam.

**Covers:**

- **The concern.** Naïve generation shows seams: adjacent chunks that sample
  noise inconsistently, or a wrap where the far edge does not meet the near
  edge (a cliff).
- **The engine's stance — sample at the world position.** A `NoiseFn` is a
  pure, deterministic scalar field of a *world* position, seeded by a `uint64_t`
  threaded through unchanged — no `rand`, no `time`, no global state
  (`src/world/Noise.h`, the purity rule from tutorial 02). Sampling at the world
  position (not a chunk-local one) is *why* adjacent chunks already agree at
  their shared edge. Everything in this tutorial builds on that one property.
- **Making a boundary match — three techniques, with trade-offs.**
  - **Transitional-swath blend** (what `demos/21-voxel-world` ships): blend a
    band at one edge toward the wrapped-around terrain with a smoothstep weight,
    giving C¹ continuity (value *and* slope) at the seam while leaving the
    interior pristine and imposing *no* constraint on world size. Insert it at
    the field that feeds everything (here, `fbm2`) so shape and biomes both
    become seamless in one place.
  - **Periodic (tileable) lattice noise**: fold the integer lattice corner
    indices modulo the period before hashing — perfectly consistent everywhere,
    but pins the world to specific period sizes.
  - **4D-torus sampling** for arbitrary periods, and where its extra cost is
    worth it.
- **Seeding discipline.** `voxel_seed_mix` to decorrelate independent fields
  (temperature vs. humidity vs. roughness) from one world seed; why the same
  seed must always yield the same world (streaming, persistence, multiplayer
  all depend on it).
- **How to verify.** Query the registered `voxelworld_height` noise (or read
  surface voxels) along both sides of a boundary and confirm they agree;
  visually, fly a wrapped world and inspect the seam band.

**Outcome:** Reader can generate terrain with no visible seams at chunk edges
or a wrap boundary, and can pick the right technique for their size constraints.

**Key references:** `plugins/voxel-world/plugin.cpp` (`fbm2` seam blend, climate
fields, lattice value noise), `src/world/Noise.h`, `include/plugin_api.h`
(`NoiseFn`, `register_noise`, `resolve_noise`, `voxel_seed_mix`).

---

## Open questions for review

1. **Numbering.** Continue as 15–17 (chosen here), or mark an explicit
   "Advanced" break in the index of the main series README?
2. **Scope of 17.** The three noise techniques could each be a short tutorial;
   folded into one here to keep the arc at three. Split if it runs long.
3. **A fourth?** Streaming budgets / LOD tuning at world scale overlaps
   tutorial 14 (Performance Tuning) — reference from 15 rather than add a 18,
   unless there is appetite for a dedicated large-scale-streaming tutorial.
