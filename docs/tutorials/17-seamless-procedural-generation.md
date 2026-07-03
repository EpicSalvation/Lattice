# Tutorial 17: Seamless Procedural Generation *(Advanced)*

Generate terrain with no visible boundaries -- not at chunk edges, not at a
decomposition boundary, and not at a wrap seam. This tutorial is the
terrain-continuity half of a wrapping world (the other half is
[Tutorial 16](16-wrapping-worlds.md)) and, more broadly, the discipline that
keeps *any* procedural world from showing its seams.

> **Advanced tutorial.** The closing chapter of the "Big, Believable Worlds" arc
> (15--17). It assumes you can write a deterministic generator
> ([Tutorial 02](02-your-first-plugin.md)) and use noise in a recipe
> ([Tutorial 04](04-composition-recipes.md)).

---

## Prerequisites

- Comfortable writing a deterministic `LayerGeneratorFn` ([Tutorial 02](02-your-first-plugin.md))
- Familiar with noise functions and recipes ([Tutorial 04](04-composition-recipes.md))
- Helpful: [Tutorial 16](16-wrapping-worlds.md) (what a wrap seam is)

The worked example is the `voxel-world` generator plugin,
[`plugins/voxel-world/plugin.cpp`](../../plugins/voxel-world/plugin.cpp).

---

## 1. The concern

Procedural terrain is sampled per-chunk, but a chunk is not an island -- its
edges must agree with its neighbours, or the world shows seams:

- **Chunk edges** -- if two adjacent chunks sample the terrain field
  differently, their shared boundary shows a step.
- **A wrap seam** -- on a torus ([Tutorial 16](16-wrapping-worlds.md)), the
  world's far edge meets its near edge; if the terrain there was generated
  independently, you get a cliff.
- **Decomposition boundaries** -- a coarse macro voxel's child grid must line up
  with its neighbour's.

All three are the same underlying question: *does the field produce consistent
values on both sides of a boundary?*

---

## 2. The engine's stance: sample at the world position

A `NoiseFn` in Lattice is a **pure, deterministic scalar field of a world
position**, seeded by a `uint64_t` threaded through unchanged -- no `rand()`, no
`time()`, no global mutable state ([`include/plugin_api.h`](../../include/plugin_api.h),
and the purity rule from [Tutorial 02](02-your-first-plugin.md)):

```cpp
using NoiseFn = float(*)(
    WorldCoord         pos,          // sampled at the WORLD position
    uint64_t           seed,         // deterministic; threaded unchanged
    const RecipeParam* params,
    size_t             param_count,
    void*              user_data
);
```

The comment on that type states the reason directly: *sampling at a world
position (not a chunk-local one) is why adjacent chunks' grids are already
seamless.* Two chunks that meet at a boundary both evaluate the same field at the
same world coordinates, so they agree there for free. **Chunk-edge seamlessness
is not something you implement; it falls out of sampling globally and
deterministically.** Everything else in this tutorial builds on that one
property.

This is also why determinism is non-negotiable: streaming (regenerate a chunk
you evicted), persistence (only store what differs from the seed), and
multiplayer (every client generates the same world) all assume the same seed and
position always yield the same voxel.

---

## 3. Making a boundary match: three techniques

Chunk edges are free. A *wrap seam* is not -- there, the field at `+half` must
equal the field at `-half`, which non-periodic noise does not satisfy. Three
techniques close that gap, with different trade-offs.

### 3a. Transitional-swath blend (what `voxel-world` ships)

Rather than change the noise, blend a band at one edge toward the
wrapped-around terrain. Because every surface and climate field in the plugin
routes through one function, `fbm2`, making *that* function wrap-aware makes
landforms and biomes seamless in one place:

```cpp
double fbm2(double x, double z, uint64_t seed, double baseFreq, int octaves) {
    if (g_wrapPeriodM <= 0.0) return fbm2_base(x, z, seed, baseFreq, octaves);
    const double P = g_wrapPeriodM, H = 0.5 * P, W = g_wrapBandM;
    const double wx = seamWeight(x, H, W), wz = seamWeight(z, H, W);
    if (wx == 0.0 && wz == 0.0) return fbm2_base(x, z, seed, baseFreq, octaves);
    // Bilinear blend toward the far edge (and, in the corner, the far corner).
    const double f00 = fbm2_base(x,     z,     seed, baseFreq, octaves);
    const double f10 = fbm2_base(x - P, z,     seed, baseFreq, octaves);
    const double f01 = fbm2_base(x,     z - P, seed, baseFreq, octaves);
    const double f11 = fbm2_base(x - P, z - P, seed, baseFreq, octaves);
    return lerp(lerp(f00, f10, wx), lerp(f01, f11, wx), wz);
}
```

`seamWeight` is a smoothstep across the band (`0` in the interior, `1` at the
edge). Because smoothstep has *zero slope* at both ends, the blend matches the
far edge in **value and slope** -- no cliff *and* no crease -- while the interior
stays byte-for-byte the native field. Measured continuity across the seam of the
shipped demo: value delta ~3e-11, slope delta ~9e-9.

The strengths of this approach: it leaves the generation algorithm untouched,
imposes **no constraint on world size**, and localises the change to one
function. Its cost: within the band the terrain is a morph between two regions
(a natural transition zone), and only the fields that route through the blended
function are covered -- see the caveats.

### 3b. Periodic (tileable) lattice noise

The plugin's value noise hashes integer lattice corners
(`lattice(ix, iy, iz, seed)`). You can make it *exactly* periodic by folding the
corner indices modulo the period, in lattice cells, before hashing:

```cpp
ix = ((ix % cellsPerPeriod) + cellsPerPeriod) % cellsPerPeriod;   // and iz
```

Now the field repeats perfectly with no special band anywhere. The catch: every
octave doubles the frequency, so *all* octaves only tile if the world period is a
whole number of lattice cells at each -- which pins the world to specific period
sizes (for this plugin's climate frequencies, multiples of tens of kilometres).
Great when you control the size; awkward when you do not. That constraint is
exactly why `voxel-world` chose the blend in 3a.

### 3c. 4D-torus sampling

For a periodic field at *any* size, sample the noise on a circle per wrapped
axis: map each world coordinate to an angle and sample at
`(cos θ, sin θ)` scaled by a radius. Two wrapped axes need a 4D noise field
(`cos θx, sin θx, cos θz, sin θz`). It periods cleanly for any size, but needs a
4D lattice (16 corners per octave instead of 8) and changes the terrain's
character. Reach for it when arbitrary size *and* perfect periodicity both
matter.

---

## 4. Blend at the right level

Where you insert continuity determines what stays consistent. In `voxel-world`
the surface *height* and the *biome* both derive from the same low-frequency
climate fields (`continentalness`, `mountainness`, `temperature`, `humidity`),
and all of them call `fbm2`. Blending at `fbm2` therefore makes the landform
shape **and** the biome selection transition together -- desert eases into
tundra across the band instead of snapping at a line.

Had you blended only the final integer height, the shape would match but biomes
could still snap (grass abruptly meeting sand at the same elevation -- a colour
seam instead of a cliff). The lesson: **insert continuity at the earliest
shared input**, not at the final output, so everything downstream inherits it.

---

## 5. Seeding discipline

Independent-looking fields should move independently, but must still be
deterministic. Derive each from the one world seed with `voxel_seed_mix` rather
than inventing separate seeds:

```cpp
double temperature(double wx, double wz, uint64_t seed) {
    return fbm2(wx, wz, voxel_seed_mix(seed, 0x7E37u), 1.0 / 512.0, 2);
}
double humidity(double wx, double wz, uint64_t seed) {
    return fbm2(wx, wz, voxel_seed_mix(seed, 0x40D17u), 1.0 / 448.0, 2);
}
```

Each field gets a decorrelated seed from the same source, so temperature and
humidity vary independently yet the whole world is reproducible from one number.
Keep that reproducibility sacred: the moment generation depends on anything but
`(seed, position)` -- a global counter, wall-clock time, iteration order -- your
world stops surviving a round-trip through streaming, save/load, or a second
client.

---

## Challenge: prove your seam matches

1. Build the demo as a wrapping world and fly to a seam
   ([Tutorial 16](16-wrapping-worlds.md)): `./build/21-voxel-world --wrap 400`.
   Confirm the terrain flows continuously across it.

2. Now *break* it: in `plugins/voxel-world/plugin.cpp`, make `fbm2` always call
   `fbm2_base` (skip the blend). Rebuild, run `--wrap 400`, and fly to the same
   seam -- you should now see the cliff the blend was hiding. Restore it.

3. Query continuity numerically: sample the registered `voxelworld_height` noise
   at a column just inside `+half` and the matching column just inside `-half`,
   and confirm the surface heights agree.

<details>
<summary>Stuck? Where to look</summary>

- The blend and `seamWeight` are in `plugins/voxel-world/plugin.cpp` (section 3a);
  the host hands it the period via `voxelworld_set_wrap` (Tutorial 16 §7).
- `voxel_seed_mix` and the `NoiseFn` contract are in `include/plugin_api.h`.
- The registered `voxelworld_height` noise is near the bottom of the plugin.

</details>

**Going further:** swap the blend (3a) for tileable lattice noise (3b) and
observe the new constraint -- the world will only look seamless at specific
sizes. That trade (any-size-with-a-band vs. fixed-size-perfectly-periodic) is the
core design decision of a wrapping generator.

---

## How to verify

```bash
cmake -B build && cmake --build build --target 21-voxel-world
./build/21-voxel-world --wrap 400
```

Fly across a seam: the terrain lines up with no cliff and biomes ease across the
transition band, while the interior is ordinary native terrain. Compared against
an infinite run (`./build/21-voxel-world`), the interior of the wrapped world is
byte-for-byte identical -- the blend touches only the seam band.

---

## Key references

| What | Where |
|------|-------|
| `fbm2` seam blend, climate fields, lattice noise | [`plugins/voxel-world/plugin.cpp`](../../plugins/voxel-world/plugin.cpp) |
| `NoiseFn` contract, `register_noise`, `resolve_noise` | [`include/plugin_api.h`](../../include/plugin_api.h) |
| `voxel_seed_mix`, `voxel_rng_next` (deterministic RNG) | [`include/plugin_api.h`](../../include/plugin_api.h) |
| Built-in noise facility | [`src/world/Noise.h`](../../src/world/Noise.h) |
| The topology half of a wrapping world | [Tutorial 16](16-wrapping-worlds.md) |
| Noise architecture (§6) | [`docs/architecture.md`](../architecture.md) |
