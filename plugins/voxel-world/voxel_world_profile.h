#pragma once

/**
 * @file voxel_world_profile.h
 * @brief Per-world generation profile for the voxel-world plugin (M19, "No Man's Voxel").
 *
 * The demos/21 "Voxel World" demo streams ONE world: the full six-biome paradise
 * with an ocean, driven by voxelworld_set_seed_ptr. The demos/22 "No Man's Voxel"
 * demo flies between SEVERAL worlds in one session, each a distinct engine World
 * with its own seed, toroidal wrap, biome set, and water. This header adds the
 * seam the multi-world demo needs: a small PROFILE the host activates before it
 * streams a given world's chunks.
 *
 * Because streaming is single-threaded and synchronous (the host generates one
 * world's chunk batch fully before moving to the next), the plugin can keep the
 * active profile in generation globals — set voxelworld_set_profile(&w.profile),
 * then load that world's chunks. This mirrors the header-shared-entry-point
 * pattern of kinematic_body.h / keyboard_mouse.h.
 *
 * The un-profiled path is untouched: a host that only ever calls
 * voxelworld_set_seed_ptr (demo 21, the determinism tests) generates the pristine
 * six-biome world exactly as before — set_seed_ptr resets these globals to their
 * demo-21 defaults, so the two entry points never interfere.
 */

#include <cstdint>

/// Biome-set selector for a world. VW_PARADISE is demo 21's full six-biome world;
/// the others are simpler worlds the No Man's Voxel demo flies out to.
enum VwBiomeMode {
    VW_PARADISE = 0,  ///< six biomes + ocean (demo 21's world)
    VW_DESERT,        ///< warm dry desert, with stony mountain ranges
    VW_SNOWY,         ///< snowy tundra, with snow-capped mountains
    VW_MOON           ///< dead grey regolith + craters; no water, trees, or ore
};

/// One world's generation profile. `period_m`/`band_m` drive the toroidal seam
/// blend (0 ⇒ no wrap, same as voxelworld_set_wrap); `has_water` inlines a flat
/// sea at/below sea level (so the multi-world demo needs no separate water plugin).
struct VwProfile {
    uint64_t seed;
    double   period_m;
    double   band_m;
    int      biome_mode;  ///< a VwBiomeMode value
    int      has_water;   ///< 1 ⇒ fill empty cells at/below sea level with water
};

#ifdef __cplusplus
extern "C" {
#endif

/// Activate `p` as the world being generated. Set before streaming a world's
/// chunks (the plugin reads these globals in its generator + feature passes).
/// Passing nullptr is a no-op.
void voxelworld_set_profile(const VwProfile* p);

#ifdef __cplusplus
}
#endif
