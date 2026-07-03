# Voxel World Demo — "AI friendliness" build metrics

This document records concrete metrics for building the **Voxel World** demo
(`demos/21-voxel-world`, plus the new `voxel-world` worldgen plugin and a determinism
test) end to end with an AI coding agent (Claude Code). It is the companion to
[`m18-mega-demo-metrics.md`](m18-mega-demo-metrics.md), and the more interesting number
here is the **comparison**: this demo is a sibling of the mega-demo built on the *same*
engine patterns, so it measures what the second trip down a well-trodden path costs.

The numbers below are the *actual* counts from a single planning-plus-implementation
session, not estimates. Token/cost figures come from the CLI's own `/cost` accounting.

## Phase metrics

| Metric | Planning | Implementation | Notes |
|---|---|---|---|
| Subagent explorations | 3 (one parallel batch) | 0 | Planning fan-out: mega-demo structure, biome/terrain gen, flight/CLI/streaming |
| Files read for ground truth | ~14 (overworld/water/trees plugins, demos 02 & 20, CMake, `plugin_api.h`, `LODManager`/`Layer`/`BgfxRenderer` headers, the mega-demo determinism test) | 0 | Reading to learn the API, not to fix mistakes |
| Clarifying-question rounds | 1 (3 questions, answered in one pass) | 0 | Locked the biome set, the world-border behaviour, and textured-vs-flat up front |
| Plan revisions | 1 (draft, one typo fix) | — | |
| **Compile-error fix cycles** | — | **0** | Both the plugin and the demo compiled on the **first** attempt |
| Warnings to clear (W4) | — | 2 (unused param + `double` truncation in the plugin) | Fixed in one pass; the rebuild was clean |
| Design corrections from the plan | — | 1 | The plan assumed the worldgen plugin would load from disk; a disk MODULE does not expose its custom C entry points (`voxelworld_biome_name`/`_set_seed_ptr`) to the host, so the demo **compiles the plugin in** (the `mob`/`kinbody` pattern) instead. Caught by reasoning **before** the first build, not by a failed compile. |
| `plugin_api.h` re-reads to fix an ABI mistake | 0 | 0 | The compile-in + `wireInPlugin` mechanism resolved from the existing pattern on the first look |
| Build iterations to green | — | 2 (plugin+demo; then +tests) — each clean | No red→green debugging loops |
| Runtime smoke tests | — | 2 (default infinite; seeded `--size 800` bounded) — no crash, 10-tile atlas built, plugins loaded | |
| New automated tests | — | 5 (same-seed determinism, different-seed divergence, origin seamlessness, layered terrain, **all six biomes reachable**) — all green; full suite **533 pass** | `tests/VoxelWorldDeterminismTest.cpp` |

### The headline AI-friendliness signal

Across a new worldgen plugin and a ~430-line demo front-end wiring streaming, textures,
input, fog, and a HUD, there were **zero compile-error fix cycles** — both C++ targets
built clean on the first attempt. The only diagnostics were two W4 warnings (an unused
parameter and a `uint64_t → double` fallback truncation), cleared in one edit. The single
non-trivial *design* correction — a disk-loaded plugin can't expose custom symbols to the
host — was caught by reasoning before the first build and resolved by reusing the engine's
existing compile-in pattern, so it never became a broken build.

## Output / leverage

| Metric | Value |
|---|---|
| New source files | 3 (`voxel-world/plugin.cpp`, `21-voxel-world/main.cpp`, `VoxelWorldDeterminismTest.cpp`) |
| New lines of code | ~1,090 net new shipped (the `/cost` line count below is session-wide, including the plan file and edit churn) |
| Existing plugins reused **unchanged** | `water` (disk-loaded flat-sea fill) |
| Engine primitives reused via the ABI / seams | `LODManager` (`desiredChunks`/`shouldEvict`/`setVerticalBand`), `World`/`Layer` streaming, `TextureManager` atlas, `register_material`/`register_feature_generator`/`register_noise`/`register_texture`/`set_material_faces`, `voxel_seed_mix`/`voxel_rng_norm`, the `BgfxRenderer` fog/far-clip/atlas/HUD seams, and raw GLFW input |
| Patterns copied-and-adapted (not linked) | the overworld plugin's inline noise + strata fill, the trees plugin's per-column decoration, demo 02's free-cam streaming loop, and the mega-demo's seed/CLI + runtime PNG-tile synthesis |
| CMake edits to wire it all | 2 small blocks (a compile-in stanza for the demo + adding the plugin source to the test target) — the GLOB build discovered the new demo, plugin, and test with no per-file edits |

The reuse ratio is again the story, but sharper than the mega-demo's: the *only* genuinely
new logic is the six-biome climate model in `voxel-world` (continuous height blend +
quantized biome identity + biome-gated decoration). The player controller, streaming loop,
texture synthesis, and seed plumbing are all lifted from patterns the mega-demo established.

## Token / cost / wall-clock

Figures from the CLI's `/cost` for the full session (planning + implementation +
verification), as reported at session close:

| Metric | Value |
|---|---|
| Total cost (USD) | **$16.16** (see cost context below) |
| Compute time (API duration) | 30 m 49 s |
| Wall-clock span | 1 h 2 m 48 s |
| Code changes counted by the session | 1,303 lines added, 19 removed (session-wide, incl. the plan file and edit churn; the *net new shipped* source is ~1,090 LOC across 3 files) |

Token usage by model (input / output / cache-read / cache-write):

| Model | Input | Output | Cache read | Cache write | Cost |
|---|---|---|---|---|---|
| claude-opus-4-8 | 40.5k | 124.3k | 15.8M | 584.1k | $16.15 |
| claude-haiku-4-5 | 1.1k | 31 | 0 | 0 | $0.0012 |

> **Cost context (important for interpreting the dollar figure).** The $16.16 is the API-
> equivalent value `/cost` attributes to the session. Date matters for any AI-cost metric:
> this was **early July 2026**, using **Claude Opus 4.8** (with Haiku 4.5 for a light
> subtask). Model capability and pricing move fast, so read these numbers as a snapshot of
> that point in time, not a fixed cost of building comparable work.

Reading the numbers: the heavy **cache read** (15.8M) relative to fresh input (40.5k) shows
almost all of the engine context was served from cache across turns rather than re-read.
Output tokens (124.3k) dominate cost — a *write-heavy* task (plugin + demo + tests + docs),
the desired shape: the agent spent its budget producing code, not hunting for how the engine
works.

## The comparison that matters

|  | Mega-Demo (M18/M18.5) | Voxel World (demo 21) |
|---|---|---|
| Total cost (USD) | $74.00 | **$16.16** |
| API compute time | 2 h 13 m | **30 m 49 s** |
| Opus output tokens | 513.6k | **124.3k** |
| Opus cache read | 84.5M | **15.8M** |
| New shipped source | ~1,550 LOC / 5 files (3 plugins + demo + tests) | ~1,090 LOC / 3 files (1 plugin + demo + test) |
| New distinctive logic | worldgen + tree placement + zombie AI | six-biome climate model |

Building the Voxel World demo cost roughly **~4.6× less** than the mega-demo (dollars),
in **~4.4× less** API time, for a comparable amount of shipped code. Some of that is smaller
scope (one worldgen plugin vs three; no combat/AI/audio), but the larger factor is **pattern
reuse**: the mega-demo paid the one-time cost of discovering how to stand up a single-terminal
streamed survival world on this engine — seed threading, the `LODManager` loop, runtime
texture synthesis, the input scheme. The second demo down that path re-derived almost none of
it; it read the mega-demo's code once (from cache, mostly) and spent its budget on the genuinely
new part. That declining marginal cost per demo — the codebase gets *cheaper* to extend as its
conventions accumulate — is exactly the AI-friendliness property the engine is designed for.

## A note on scope honesty

Like the mega-demo, this demo is deliberately **not** a kitchen sink. It is a *fly-only
explorer*: no mining, building, combat, mobs, or audio — those are the mega-demo's job. Its
one job is worldgen breadth and seamless streaming, so the engineering (and the metrics above)
concentrate on the biome generator and the infinite/bounded streaming toggle rather than on
re-demonstrating features other demos already own. The biome model itself makes an honest
tradeoff worth recording: terrain **height** is a continuous blend of climate fields (so chunk
borders never seam), while biome **identity** is a hard quantization used only for surface
material and decoration — which means biome *material* transitions are visible seams, exactly
as they are in Minecraft, rather than blended. That is a deliberate choice, not a gap.
