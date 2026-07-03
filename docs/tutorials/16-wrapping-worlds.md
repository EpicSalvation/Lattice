# Tutorial 16: Wrapping (Toroidal) Worlds *(Advanced)*

Turn a finite world into one with no edges: fly past one side and arrive at the
other, like circumnavigating a planet. This tutorial covers the *topology* half
of a wrapping world -- folding the camera and chunk grid onto a torus and
rendering it seamlessly -- and hands off the *terrain-continuity* half to
[Tutorial 17](17-seamless-procedural-generation.md).

> **Advanced tutorial.** This is part of the "Big, Believable Worlds" advanced
> arc (15--17). It assumes you are comfortable with the 01--14 material: writing
> a plugin ([02](02-your-first-plugin.md)), a layer config, and the
> load/evict streaming loop a demo runs each frame. It also builds on the
> coordinate-space concepts in [Tutorial 15](15-large-worlds-and-coordinate-space.md).

---

## Prerequisites

- Engine built and running ([Tutorial 01](01-hello-voxel.md))
- Comfortable writing a plugin and its deterministic generator ([Tutorial 02](02-your-first-plugin.md))
- Familiar with a demo's chunk streaming loop ([Tutorial 07](07-multi-layer-worlds.md))
- Recommended: [Tutorial 15](15-large-worlds-and-coordinate-space.md) (floating origin, coordinate limits)

The worked example throughout is `demos/21-voxel-world`, which supports three
world topologies selectable at launch: the default **infinite** world, a
`--size N` **hard border**, and the `--wrap N` **torus** this tutorial is about.

---

## 1. The concern

A finite world has an edge, and an edge is a problem. Either the player falls
off it, or you stop them with an invisible wall (the `--size` hard border) --
and a wall in the middle of open terrain breaks the illusion. A planet has no
edge: walk far enough in one direction and you return to where you started.

We want that planet feel from a *finite* amount of world: a fixed square that
loops back on itself in both horizontal directions. Mathematically that shape is
a **torus** -- the world's `+x` edge is identified with its `-x` edge, and
likewise for `z`.

Two things have to be true for it to work, and they are surprisingly separate:

1. **Position and streaming** must loop -- cross the seam and your coordinates,
   the chunks that load, and what gets drawn all continue from the far side.
2. **Terrain must match** at the seam -- the ground on one side has to line up
   with the ground on the other, or you get a cliff where the edges meet.

This tutorial does (1). Tutorial 17 does (2). Understanding *why they are
separate* is the real lesson.

---

## 2. The engine's stance: topology is the host's job

The engine's chunk grid is a plain, unbounded `int32` lattice. It has no notion
of a world that wraps -- exactly as it has no notion of the `--size` hard
border. **World topology lives in the host (your demo/game), not the engine
core.**

This is deliberate. A coordinate wrap is a statement about the *grid*, not about
the *content*. Whether the world loops every 800 metres is a game-design choice
the engine has no business baking into `World` or `ChunkCoordMath`. So, just
like `--size`, wrapping is implemented entirely on top of the engine's ordinary
API -- the engine never learns the world is round.

The reusable math lives in a small, header-only helper you can lift into any
demo: [`demos/common/WorldWrap.h`](../../demos/common/WorldWrap.h). The planned
M19 "No Man's Voxel" demo shares it.

---

## 3. The two-party contract

Wrapping splits into two jobs, and **neither party can do the other's**:

| Concern | Owner | What it does |
|---------|-------|--------------|
| **Topology** | the host (demo) | fold position + chunk coords onto the torus; draw each chunk at the image nearest the camera |
| **Seam continuity** | the generator (plugin) | make its terrain *match* where the two edges meet |

The host cannot make the terrain line up, because only the generator knows how
terrain is synthesized. The generator cannot fold coordinates or decide what to
stream, because that is the host's loop. They cooperate through exactly **one
shared number: the world period.** The host computes it and tells the generator;
the generator makes its noise continuous across it.

Keep this contract in mind -- it is why even a hypothetical engine-level
wrapping feature would still need the generator to cooperate.

---

## 4. Folding the world: `WorldWrap.h`

The whole topology half is three pure functions on a `Torus`, defined in
[`demos/common/WorldWrap.h`](../../demos/common/WorldWrap.h):

```cpp
struct Torus {
    int    periodChunks    = 0;    // whole chunks per axis; 0 = not wrapping (even)
    double chunkWorldSizeM = 0.0;  // world size of one chunk, in metres

    double periodM() const { return periodChunks * chunkWorldSizeM; }
    double halfM()   const { return 0.5 * periodM(); }

    // Fold a world coordinate into the canonical domain [-half, +half).
    double wrapCoordM(double c) const {
        const double P = periodM();
        return c - P * std::floor((c + halfM()) / P);
    }

    // Fold a chunk index into [-period/2, +period/2). periodChunks is even, so
    // the range is symmetric about the origin.
    int wrapChunk(int c) const {
        int m = c % periodChunks;
        if (m < 0) m += periodChunks;
        if (m >= periodChunks / 2) m -= periodChunks;
        return m;
    }

    // The periodic image of a chunk-origin coordinate nearest the camera.
    double nearestOriginM(double canonOriginM, double camM) const {
        const double P = periodM();
        return canonOriginM - P * std::round((canonOriginM - camM) / P);
    }
};
```

Three ideas, one per function:

- **`wrapCoordM`** keeps the camera position inside the canonical domain
  `[-half, +half)`. When you fly past `+half`, you reappear at `-half`.
- **`wrapChunk`** folds a chunk index onto the torus. This is the crucial trick:
  the chunk just west of the seam and the chunk just east of it fold to the
  **same canonical chunk**, so the two sides literally reuse *byte-for-byte
  identical chunk data*. The wrap does not copy terrain; it *identifies* the two
  edges as the same place.
- **`nearestOriginM`** is for rendering. A canonical chunk lives at one
  position, but on a torus it should be drawn wherever its periodic image is
  closest to the camera -- so a chunk stored at `-half` renders at `+half` when
  you are standing near the `+` edge, tiling across the seam.

---

## 5. The constraints, and why

Two rules on the period fall directly out of the math above:

1. **The period is a whole, even number of chunks.** *Whole* so the two seam
   sides land on the same chunk grid (fractional chunks would not line up).
   *Even* so the origin-centred canonical range `[-period/2, +period/2)` is
   symmetric -- which puts the origin (where the player spawns) in pristine
   interior terrain, far from the seam.

2. **The period is at least the streaming diameter** (`2 * view_distance + 2`
   chunks). If the world were narrower than the ring of chunks the camera loads,
   a single chunk would need to appear on *both* sides of you at once -- and one
   mesh drawn at one nearest-image position cannot do that. Enforcing a minimum
   period sidesteps the whole problem.

The demo snaps the requested `--wrap N` up to satisfy both:

```cpp
const int viewDist  = layerConfig.findLayer("terrain")->view_distance_chunks;
const int minPeriod = 2 * viewDist + 2;
int periodChunks = std::llround(2.0 * wrapHalfReqM / chunkWorldSizeM);
periodChunks = std::max(periodChunks, minPeriod);
periodChunks += (periodChunks & 1);                 // force even
worldwrap::Torus torus{periodChunks, chunkWorldSizeM};
```

Concretely, `--wrap 400` with 32 m chunks and `view_distance_chunks: 6` snaps to
a **26-chunk (832 m) period** (25 rounded up to even 26; comfortably above the
minimum of 14).

---

## 6. Wiring the topology into the loop

With a `Torus` in hand, three edits turn the ordinary streaming loop into a
wrapping one. All three are in
[`demos/21-voxel-world/main.cpp`](../../demos/21-voxel-world/main.cpp).

### Wrap the camera position

After integrating movement, fold the new position back into the domain instead
of clamping it to a border:

```cpp
if (bounded) {                       // --size: hard border
    p.x = std::clamp(p.x, -halfSizeM, halfSizeM);
    p.z = std::clamp(p.z, -halfSizeM, halfSizeM);
} else if (torus.enabled()) {        // --wrap: loop to the opposite edge
    p.x = torus.wrapCoordM(p.x);
    p.z = torus.wrapCoordM(p.z);
}
```

### Fold what you stream

The camera stays in the canonical domain, but its ring of desired chunks can
spill over the seam. Fold each desired chunk onto the torus before loading it,
and -- because LOD's raw distance metric is meaningless on folded keys -- drive
eviction off the desired set directly:

```cpp
const auto ring = lod.desiredChunks(center, "terrain");
std::unordered_set<ChunkCoord, ChunkCoordHash> desired;
for (const ChunkCoord& c : ring) desired.insert(wrapCC(c));   // wrapCC folds x,z

for (const ChunkCoord& c : ring)
    if (loadChunk(wrapCC(c)) && ++loaded >= kLoadsPerFrame) break;

for (const auto& kv : meshes)
    if (!desired.count(kv.first)) toEvict.push_back(kv.first);
```

Because two folded coordinates only collide when the period is smaller than the
ring -- which the minimum-period rule forbids -- the fold is one-to-one here, so
no chunk is processed twice.

### Draw each chunk at its nearest image

A resident chunk's canonical origin may be a whole world away from the camera.
Shift it to the periodic image nearest the camera so it tiles across the seam:

```cpp
WorldCoord origin = chunk->origin();
if (torus.enabled())
    origin = WorldCoord(torus.nearestOriginM(origin.value.x, camPos.value.x),
                        origin.value.y,
                        torus.nearestOriginM(origin.value.z, camPos.value.z));
renderer.renderChunk(kv.second, origin, voxelSize);
```

Note that `y` is never wrapped -- only the two horizontal axes loop.

---

## 7. Handing the period to the generator

Topology is done -- but if you stopped here, the terrain on the two sides of the
seam would not match, and you would see a cliff. That is the generator's job
(Tutorial 17), and the host triggers it with the one shared number:

```cpp
const double bandM = std::max(chunkWorldSizeM, torus.periodM() * 0.06);
voxelworld_set_wrap(torus.periodM(), bandM);   // period + seam blend-band width
```

Briefly, so this tutorial stands on its own: the `voxel-world` plugin makes its
terrain periodic *not* by rewriting its noise, but by blending a transitional
band (width `bandM`) at one edge toward the wrapped-around terrain. Because every
surface field routes through one function (`fbm2`), that single blend makes both
the landforms and the biomes continuous. The blend uses a smoothstep weight, so
the seam matches in **both value and slope** -- no cliff *and* no crease -- while
the interior stays byte-for-byte the native terrain. Measured continuity across
the seam: value delta ~3e-11, slope delta ~9e-9.

The key architectural point for *this* tutorial: the host never touches noise,
and the generator never touches coordinates. They meet at `period`.

---

## 8. Caveats and limits

- **Caves and other 3D noise are not seam-blended.** The blend covers the
  surface fields; an underground carve can differ across the seam. It rarely
  meets the surface seam line, so it is seldom visible -- but a game built around
  caves at the seam would need to extend the blend (Tutorial 17).
- **Discrete features can clip at the exact seam** -- a tree straddling the seam
  column has no partner on the far side, the same way features clip at any chunk
  edge today.
- **Coordinates jump at the seam.** Cross `+half` and your reported `x` snaps to
  `-half`. That is correct (it is longitude wrapping), but any code that assumes
  monotonic position -- trails, dead-reckoning, "how far have I travelled" --
  must account for it.
- **The world truly repeats.** A torus is finite; fly one lap and you see the
  same terrain again. That is the point, but set the period large enough that a
  lap feels long.

---

## Challenge: make a small planet

1. Build and run the demo as a tight torus:

   ```bash
   ./build/21-voxel-world --wrap 300
   ```

   Read the startup log for the snapped period (it will be larger than 300 asked
   for -- why?).

2. Fly straight in one direction (hold `W`) and watch the HUD `xyz`. Confirm the
   coordinate wraps from `+half` to `-half` while the terrain flows past
   continuously -- no cliff, no void.

3. Fly a full lap and confirm you recognise the terrain you started on.

4. Now run `--wrap 50`. What period do you actually get, and why can it not be
   smaller?

<details>
<summary>Stuck? Where to look</summary>

- The snap-up logic and the minimum-period rule are in section 5 (code in
  `demos/21-voxel-world/main.cpp`, just after the layer is created).
- The three folding functions are in `demos/common/WorldWrap.h` (section 4).
- If terrain looks torn at the seam, the generator did not get the period --
  check the `voxelworld_set_wrap` call (section 7).

</details>

**Going further:** compare `--wrap 300` with `--size 300`. Same finite world,
two topologies -- one loops, one stops you at a wall. That contrast is the whole
point of keeping topology in the host: the *engine* is identical in both runs.

---

## How to verify

```bash
cmake -B build && cmake --build build --target 21-voxel-world

# A wrapping world (torus):
./build/21-voxel-world --wrap 400

# The same size as a hard border, for contrast:
./build/21-voxel-world --size 400
```

In the `--wrap` run, the HUD shows `wrapping`, and flying across an edge loops
you to the opposite side with terrain that lines up. In the `--size` run, the
HUD shows `bounded` and you cannot fly past the edge at all. The startup log
prints the snapped period, e.g. `Wrapping world: period 832 m (26 chunks), seam
band 49 m.`

---

## Key references

| What | Where |
|------|-------|
| Torus fold math (position, chunk, render image) | [`demos/common/WorldWrap.h`](../../demos/common/WorldWrap.h) |
| Wrap wiring: args, snap, stream fold, render offset | [`demos/21-voxel-world/main.cpp`](../../demos/21-voxel-world/main.cpp) |
| Seam blend + `voxelworld_set_wrap` | [`plugins/voxel-world/plugin.cpp`](../../plugins/voxel-world/plugin.cpp) |
| Seam continuity technique (the other half) | [Tutorial 17](17-seamless-procedural-generation.md) |
| Coordinate space, floating origin, world limits | [Tutorial 15](15-large-worlds-and-coordinate-space.md) |
| Floating-origin architecture rule | [`docs/architecture.md`](../architecture.md) §1 |
