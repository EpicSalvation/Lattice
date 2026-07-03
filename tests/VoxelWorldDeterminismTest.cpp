// Verifies the "Voxel World" demo's headline guarantees (demos/21): the world
// seed deterministically drives generation, so the SAME seed regenerates the SAME
// world and a DIFFERENT seed differs (ARCHITECTURE §4), the generator is seamless
// across chunk origins, and the six-biome climate quantization actually reaches
// all six biomes.
//
// Exercises the real voxel-world worldgen plugin through the terminal terrain
// generator + its cave/ore/decoration feature overlays (the same path the demo
// uses), compiled in via VOXELWORLD_COMPILED_IN.

#include "core/PluginManager.h"
#include "world/Voxel.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

extern "C" int         voxelworld_plugin_init(PluginContext* ctx);
extern "C" void        voxelworld_set_seed_ptr(uint64_t* ptr);
extern "C" const char* voxelworld_biome_name(double wx, double wz);

namespace {

struct Gen {
    PluginManager    pm;
    LayerGeneratorFn terrain = nullptr;
    uint64_t         seed = 0;
    Gen() {
        pm.wireInPlugin(voxelworld_plugin_init);
        for (const auto& g : pm.layerGenerators())
            if (g.layer_name == "terrain") terrain = g.fn;
    }
    std::vector<Voxel> world(WorldCoord origin, int n, uint64_t s) {
        seed = s;
        voxelworld_set_seed_ptr(&seed);
        std::vector<Voxel> v(static_cast<size_t>(n) * n * n, Voxel::empty());
        terrain(origin, n, v.data(), &seed);
        for (const auto& f : pm.featureGenerators())
            if (f.fn) f.fn(origin, 1.0, n, v.data(), nullptr, 0, seed, f.user_data);
        return v;
    }
};

bool same(const Voxel& a, const Voxel& b) {
    return a.material.palette_index == b.material.palette_index &&
           a.material.density == b.material.density;
}
int diffCount(const std::vector<Voxel>& a, const std::vector<Voxel>& b) {
    int d = 0;
    for (size_t i = 0; i < a.size(); ++i) if (!same(a[i], b[i])) ++d;
    return d;
}

constexpr int    kN = 32;
const WorldCoord kOrigin(0.0, 0.0, 0.0);

}  // namespace

TEST(VoxelWorldDeterminism, SameSeedRegeneratesIdenticalWorld) {
    Gen g;
    ASSERT_NE(g.terrain, nullptr);
    const std::vector<Voxel> a = g.world(kOrigin, kN, 12345u);
    const std::vector<Voxel> b = g.world(kOrigin, kN, 12345u);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(diffCount(a, b), 0) << "the same seed must regenerate the same world byte-for-byte";
}

TEST(VoxelWorldDeterminism, DifferentSeedProducesDifferentWorld) {
    Gen g;
    ASSERT_NE(g.terrain, nullptr);
    const std::vector<Voxel> a = g.world(kOrigin, kN, 12345u);
    const std::vector<Voxel> b = g.world(kOrigin, kN, 99999u);
    EXPECT_GT(diffCount(a, b), 0) << "a different seed should drive a visibly different world";
}

TEST(VoxelWorldDeterminism, ChunkOriginIsSeamless) {
    Gen g;
    ASSERT_NE(g.terrain, nullptr);
    // The base terrain generator samples at world coords, so a sub-region of a
    // bigger chunk must match a same-origin smaller chunk. (Only the base fill is
    // origin-seamless; feature overlays clip at the grid bound, so compare terrain
    // only by skipping the top decoration band via a low, sub-surface slab.)
    const int small = 16, big = 32;
    const std::vector<Voxel> a = g.world(kOrigin, small, 7u);
    const std::vector<Voxel> b = g.world(kOrigin, big,   7u);
    for (int z = 0; z < small; ++z)
        for (int y = 0; y < small; ++y)   // heights start ~7; y<16 is base strata
            for (int x = 0; x < small; ++x)
                EXPECT_TRUE(same(a[x + small * (y + small * z)],
                                 b[x + big   * (y + big   * z)]))
                    << "mismatch at (" << x << "," << y << "," << z << ")";
}

TEST(VoxelWorldDeterminism, AllSixBiomesAreReachable) {
    uint64_t seed = 0xA11CE5EEDull;   // the demo's default seed
    voxelworld_set_seed_ptr(&seed);
    std::set<std::string> seen;
    for (int z = -8000; z <= 8000; z += 64)
        for (int x = -8000; x <= 8000; x += 64)
            seen.insert(voxelworld_biome_name(x + 0.5, z + 0.5));
    for (const char* b : {"Ocean", "Plains", "Forest", "Desert", "Mountains", "Snowy Tundra"})
        EXPECT_TRUE(seen.count(b) > 0) << "biome '" << b << "' never appears for the default seed";
}

TEST(VoxelWorldDeterminism, ProducesLayeredTerrain) {
    Gen g;
    ASSERT_NE(g.terrain, nullptr);
    const std::vector<Voxel> v = g.world(kOrigin, kN, 12345u);
    bool air = false, stone = false, surface = false;
    for (const auto& vox : v) {
        if (vox.isEmpty()) { air = true; continue; }
        const uint8_t p = vox.material.palette_index;
        if (p == 1) stone = true;                       // stone bulk
        if (p == 2 || p == 6 || p == 7) surface = true; // grass / sand / snow cap
    }
    EXPECT_TRUE(air);
    EXPECT_TRUE(stone);
    EXPECT_TRUE(surface);
}
