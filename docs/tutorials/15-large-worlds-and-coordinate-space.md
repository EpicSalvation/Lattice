# Tutorial 15: Large Worlds and Coordinate Space *(Advanced)*

How big can a world honestly be, and how does a position stay precise when it
is? This tutorial covers the floating-origin discipline, what a 64-bit `double`
actually buys you, and where an "infinite" world *really* ends -- which is not
where most people guess.

> **Advanced tutorial.** This opens the "Big, Believable Worlds" advanced arc
> (15--17). It assumes the 01--14 grounding: a plugin, a layer config, and a
> demo's streaming loop. It is the foundation for
> [Tutorial 16 (Wrapping Worlds)](16-wrapping-worlds.md) and
> [Tutorial 17 (Seamless Procedural Generation)](17-seamless-procedural-generation.md).

---

## Prerequisites

- Engine built and running ([Tutorial 01](01-hello-voxel.md))
- Comfortable writing a plugin ([Tutorial 02](02-your-first-plugin.md))
- Familiar with a demo's chunk streaming loop ([Tutorial 07](07-multi-layer-worlds.md))

The worked example is `demos/21-voxel-world`, which flies a streamed world and
can teleport to extreme coordinates on the number keys.

---

## 1. The concern

Two problems appear only once a world gets large, and both are easy to miss
until they bite:

1. **Precision loss.** A 32-bit `float` has about 7 significant decimal digits.
   At 100 km from the origin it can no longer represent sub-metre positions --
   the camera visibly snaps to a grid and geometry shimmers.
2. **"Infinite" is never infinite.** Some integer *always* overflows first.
   The question is only *which one, and at what distance* -- and whether you
   hit it as a graceful limit or as undefined behaviour.

An engine that ignores both feels fine in a demo and falls apart in a real
world. Lattice addresses the first with a type rule and the second with an
honest, documented ceiling.

---

## 2. The engine's stance: floating origin

Every world-space position in the engine is a `WorldCoord` -- a wrapper over a
`glm::dvec3` (three 64-bit doubles), defined in
[`include/WorldCoord.h`](../../include/WorldCoord.h). The rule, enforced by the
type system, is: **never store a world position as `float`.** The one and only
narrowing to single precision is:

```cpp
// The ONLY permitted conversion to float. Subtracts the camera first, so the
// result is a small, camera-relative offset that float represents precisely.
glm::vec3 toLocalFloat(const WorldCoord& cameraOrigin) const {
    return glm::vec3(value - cameraOrigin.value);
}
```

This is the *floating origin* pattern (see [`docs/architecture.md`](../architecture.md)
§1). World arithmetic happens in double; only at the very last step -- handing
vertices to the GPU, which wants float -- do we subtract the camera position and
narrow. The GPU therefore never sees a huge absolute coordinate, only a small
offset from the camera, so it stays precise no matter how far the camera is from
the origin.

The payoff of defining `WorldCoord` early: precision is a property of the type,
not something you can forget at one call site. Retrofitting it later means
auditing every position in the codebase.

---

## 3. What a `double` actually buys you

A `double` has a 52-bit mantissa, so its step size (the ULP -- unit in the last
place) at a value in `[2^e, 2^(e+1))` is `2^(e-52)`. Walk the numbers at the
distances that matter:

| Distance from origin | ULP (position resolution) |
|----------------------|---------------------------|
| 30,000,000 m (the classic Minecraft border) | ≈ 4 nm (`2^-28`; never worse than ~7 nm in that range) |
| ~4 × 10¹² m | ≈ 1 mm -- first point sub-millimetre precision is lost |
| ~4.5 × 10¹⁵ m | ≈ 1 m -- a voxel-sized step; roughly half a light-year |

The headline: at the Minecraft border, a `double` position is precise to a few
*nanometres*. It does not degrade to even a millimetre until ~4 × 10¹² m --
about 140,000× past that border. **Double precision is not the limit anywhere a
player will ever go.** So what is?

---

## 4. Where the real ceiling is

The first wall is an *integer* type, not the float. Follow the coordinate types:

- **`WorldCoord`** (double) -- position. As shown, good to ~4.5 × 10¹⁵ m before
  precision reaches a metre. Not the limit.
- **`ChunkCoord`** is `int32` ([`src/world/Chunk.h`](../../src/world/Chunk.h)).
  A chunk index is `floor(worldPos / chunkWorldSize)`. With 32 m chunks, the
  index overflows `int32` at `2,147,483,647 × 32 ≈ 6.9 × 10¹⁰ m` -- about
  **2,300× past the Minecraft border**. Cross it and `worldToChunk`'s
  `static_cast<int32_t>` is signed overflow (undefined behaviour): chunk lookups
  wrap and the world tears. **This is the true first wall.**
- **`VoxelCoord`** is `int64`
  ([`src/world/ChunkCoordMath.h`](../../src/world/ChunkCoordMath.h)) -- *by
  design*, because a global voxel index is `chunkCoord × chunkSize + local`,
  which would overflow `int32` well inside the double world extent. Using 64
  bits keeps the voxel address space well ahead of everything else.

So the ordering of limits is: `int32` chunk grid (≈ 7 × 10¹⁰ m) first, then
double precision (astronomically far), with the `int64` voxel index never the
bottleneck. The chunk grid is the number to respect.

---

## 5. The pattern: clamp inside the real limit, teleport to inspect it

`demos/21-voxel-world` turns the analysis above into two concrete practices.

**Clamp horizontal position safely short of the int32 wall.** The demo defines a
hard engine limit and clamps every frame's position to it, so free-flight can
never reach the overflow:

```cpp
// 1000x the Minecraft border, and ~2.3x inside the int32 chunk-overflow
// distance -- far past anywhere a player goes, safely short of UB.
constexpr double kEngineLimitM = 3.0e10;
...
p.x = std::clamp(p.x, -kEngineLimitM, kEngineLimitM);
p.z = std::clamp(p.z, -kEngineLimitM, kEngineLimitM);
```

This is the honest version of "infinite": the world streams freely up to a limit
chosen from the actual failure distance, rather than pretending there is none and
overflowing.

**Teleport to inspect precision at range.** Flying to 30,000,000 m at any sane
speed would take hours, so the demo binds teleport presets to the number keys
(1--6), escalating from the origin to the Minecraft border (key 5) and the engine
edge (key 6). Each preset re-settles the camera on the surface at the
destination:

```cpp
double tx = kTeleportPresetsM[i];      // e.g. 30,000,000 for the MC border
if (bounded) tx = std::clamp(tx, -halfSizeM, halfSizeM);
tx = std::clamp(tx, -kEngineLimitM, kEngineLimitM);
camPos = settleAt(tx + 0.5, 0.5);      // load chunks there, drop onto terrain
```

Jump to the border and fly around: motion stays perfectly smooth, because at 4 nm
resolution the double has nanometres to spare and the renderer only ever sees
camera-relative floats.

A `--size N` hard border (a clamp in chunk coordinates) is the third practice --
a deliberately small world, covered as a contrast in
[Tutorial 16](16-wrapping-worlds.md).

---

## Challenge: find your own wall

1. Run the demo and teleport to the Minecraft border (key `5`) and the engine
   edge (key `6`). Watch the HUD `xyz` and confirm motion is smooth at both.

2. Compute the int32 chunk-overflow distance for *your* chunk size. For 32 m
   chunks it is `2^31 × 32 ≈ 6.9 × 10¹⁰ m`; halve the chunk size and the wall
   halves too. Confirm `kEngineLimitM` sits comfortably inside it.

3. Predict the distance at which a `double` position first loses millimetre
   precision (hint: solve `2^(e-52) = 10^-3`), and confirm it is far beyond the
   int32 wall -- i.e. the integer grid, not the float, is what you must respect.

<details>
<summary>Stuck? Where to look</summary>

- `kEngineLimitM`, `kMcBorderM`, and the teleport presets are near the top of
  `demos/21-voxel-world/main.cpp`; the clamp is in the movement block.
- `ChunkCoord` is `int32` in `src/world/Chunk.h`; `worldToChunk` (the cast that
  overflows) is in `src/world/ChunkCoordMath.h`.
- The ULP table is section 3; the type ordering is section 4.

</details>

**Going further:** temporarily shrink the chunk size in the layer config and
recompute the wall -- smaller chunks mean a nearer int32 overflow. This is a real
trade-off: fine chunks stream and cull better but bring the coordinate ceiling
closer.

---

## How to verify

```bash
cmake -B build && cmake --build build --target 21-voxel-world
./build/21-voxel-world
```

Press `5` to teleport to the 30,000,000 m Minecraft border, then fly with `WASD`
and Space/Shift. The HUD `xyz` reads ~30,000,000 while motion stays smooth --
proof the `double` position and the camera-relative render path hold precision at
range. Press `6` for the engine edge (`3 × 10¹⁰ m`); the clamp will not let you
past it.

---

## Key references

| What | Where |
|------|-------|
| `WorldCoord`, `toLocalFloat` (floating origin) | [`include/WorldCoord.h`](../../include/WorldCoord.h) |
| `ChunkCoord` (int32), the first wall | [`src/world/Chunk.h`](../../src/world/Chunk.h) |
| `VoxelCoord` (int64), `worldToChunk` | [`src/world/ChunkCoordMath.h`](../../src/world/ChunkCoordMath.h) |
| `kEngineLimitM`, teleport presets, clamp | [`demos/21-voxel-world/main.cpp`](../../demos/21-voxel-world/main.cpp) |
| Floating-origin architecture rule | [`docs/architecture.md`](../architecture.md) §1 |
| Wrapping a finite world (next in the arc) | [Tutorial 16](16-wrapping-worlds.md) |
