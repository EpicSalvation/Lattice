// Verifies the "No Man's Voxel" demo's per-world profile path (demos/22): the
// voxel-world worldgen plugin, driven by a VwProfile (voxel_world_profile.h),
// generates each of the four worlds deterministically, selects the right biome set
// per mode, keeps the moon dead (no water / trees / ore), and inlines a flat sea
// only when a profile asks for it. Also confirms the un-profiled path is untouched
// (that guarantee is pinned by VoxelWorldDeterminismTest, which shares this binary).
//
// Exercises the real plugin compiled in via VOXELWORLD_COMPILED_IN, the same path
// the demo uses.

#include "core/PluginManager.h"
#include "world/Voxel.h"

#include "voxel_world_profile.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

extern "C" int         voxelworld_plugin_init(PluginContext* ctx);
extern "C" void        voxelworld_set_profile(const VwProfile* p);
extern "C" void        voxelworld_set_seed_ptr(uint64_t* ptr);
extern "C" const char* voxelworld_biome_name(double wx, double wz);

namespace {

// Palette slots the plugin registers (see plugins/voxel-world/plugin.cpp).
constexpr uint8_t kStoneIdx   = 1;
constexpr uint8_t kLeavesIdx  = 4;
constexpr uint8_t kWaterIdx   = 5;
constexpr uint8_t kLogIdx     = 8;
constexpr uint8_t kCactusIdx  = 9;
constexpr uint8_t kIronIdx    = 11;
constexpr uint8_t kBedrockIdx = 14;

constexpr int kN = 32;

VwProfile prof(uint64_t seed, VwBiomeMode mode, bool water, double period = 0.0) {
    return VwProfile{seed, period, period > 0.0 ? period * 0.06 : 0.0,
                     static_cast<int>(mode), water ? 1 : 0};
}

struct Gen {
    PluginManager    pm;
    LayerGeneratorFn terrain = nullptr;
    uint64_t         seed = 0;
    Gen() {
        pm.wireInPlugin(voxelworld_plugin_init);
        for (const auto& g : pm.layerGenerators())
            if (g.layer_name == "terrain") terrain = g.fn;
    }
    // Generate a world block under the given profile (terrain fill + cave/ore/
    // decoration overlays — the exact path the demo streams).
    std::vector<Voxel> world(WorldCoord origin, int n, const VwProfile& p) {
        VwProfile prof = p;
        voxelworld_set_profile(&prof);
        seed = p.seed;
        std::vector<Voxel> v(static_cast<size_t>(n) * n * n, Voxel::empty());
        terrain(origin, n, v.data(), &seed);
        for (const auto& f : pm.featureGenerators())
            if (f.fn) f.fn(origin, 1.0, n, v.data(), nullptr, 0, seed, f.user_data);
        return v;
    }
    bool contains(const std::vector<Voxel>& v, uint8_t palette) const {
        for (const auto& x : v)
            if (!x.isEmpty() && x.material.palette_index == palette) return true;
        return false;
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

// Find a chunk-aligned origin whose local (0,0) column is an ocean column under the
// paradise profile, so we can prove has_water inlines a sea there.
bool findOceanOrigin(uint64_t seed, WorldCoord& out) {
    VwProfile p = prof(seed, VW_PARADISE, true);
    voxelworld_set_profile(&p);
    for (int z = -4000; z <= 4000; z += kN)
        for (int x = -4000; x <= 4000; x += kN)
            if (std::string(voxelworld_biome_name(x + 0.5, z + 0.5)) == "Ocean") {
                out = WorldCoord(static_cast<double>(x), 0.0, static_cast<double>(z));
                return true;
            }
    return false;
}

const WorldCoord kOrigin(0.0, 0.0, 0.0);

}  // namespace

TEST(NoMansVoxelDeterminism, EachModeRegeneratesIdentically) {
    Gen g;
    ASSERT_NE(g.terrain, nullptr);
    for (VwBiomeMode mode : {VW_PARADISE, VW_DESERT, VW_SNOWY, VW_MOON}) {
        const VwProfile p = prof(4242u, mode, mode == VW_PARADISE);
        const std::vector<Voxel> a = g.world(kOrigin, kN, p);
        const std::vector<Voxel> b = g.world(kOrigin, kN, p);
        EXPECT_EQ(diffCount(a, b), 0)
            << "mode " << static_cast<int>(mode) << " must regenerate byte-for-byte";
    }
}

TEST(NoMansVoxelDeterminism, ProfileSelectsBiomeSet) {
    Gen g;
    // Paradise reaches many biomes; the simpler worlds are restricted to their
    // dominant biome plus mountains; the moon reports its neutral bare biome.
    const uint64_t seed = 0xA11CE5EEDull;

    auto biomesFor = [&](VwBiomeMode mode) {
        VwProfile p = prof(seed, mode, false);
        voxelworld_set_profile(&p);
        std::set<std::string> seen;
        for (int z = -4000; z <= 4000; z += 64)
            for (int x = -4000; x <= 4000; x += 64)
                seen.insert(voxelworld_biome_name(x + 0.5, z + 0.5));
        return seen;
    };

    const std::set<std::string> paradise = biomesFor(VW_PARADISE);
    EXPECT_GE(paradise.size(), 4u) << "paradise should span many biomes";

    const std::set<std::string> desert = biomesFor(VW_DESERT);
    for (const auto& b : desert)
        EXPECT_TRUE(b == "Desert" || b == "Mountains")
            << "desert world produced an unexpected biome: " << b;
    EXPECT_TRUE(desert.count("Desert") > 0);

    const std::set<std::string> snowy = biomesFor(VW_SNOWY);
    for (const auto& b : snowy)
        EXPECT_TRUE(b == "Snowy Tundra" || b == "Mountains")
            << "snowy world produced an unexpected biome: " << b;
    EXPECT_TRUE(snowy.count("Snowy Tundra") > 0);
}

TEST(NoMansVoxelDeterminism, MoonIsDeadStoneNoWaterTreesOre) {
    Gen g;
    ASSERT_NE(g.terrain, nullptr);
    // Scan several origins so we exercise plenty of moon surface.
    bool anyStone = false, anyBedrock = false;
    for (int i = 0; i < 6; ++i) {
        const WorldCoord o(static_cast<double>(i * kN), 0.0, static_cast<double>(i * kN));
        const std::vector<Voxel> v = g.world(o, kN, prof(777u, VW_MOON, /*water=*/false));
        anyStone   |= g.contains(v, kStoneIdx);
        anyBedrock |= g.contains(v, kBedrockIdx);
        EXPECT_FALSE(g.contains(v, kWaterIdx))  << "moon must have no water";
        EXPECT_FALSE(g.contains(v, kLogIdx))    << "moon must have no tree trunks";
        EXPECT_FALSE(g.contains(v, kLeavesIdx)) << "moon must have no leaves";
        EXPECT_FALSE(g.contains(v, kCactusIdx)) << "moon must have no cacti";
        EXPECT_FALSE(g.contains(v, kIronIdx))   << "moon must have no ore";
    }
    EXPECT_TRUE(anyStone)   << "the moon should be built of stone regolith";
    EXPECT_TRUE(anyBedrock) << "the moon should still have a bedrock floor";
}

TEST(NoMansVoxelDeterminism, HasWaterInlinesSeaOnlyWhenSet) {
    Gen g;
    ASSERT_NE(g.terrain, nullptr);
    WorldCoord ocean(0.0, 0.0, 0.0);
    ASSERT_TRUE(findOceanOrigin(0xA11CE5EEDull, ocean))
        << "expected an ocean column for the default seed";

    const std::vector<Voxel> wet =
        g.world(ocean, kN, prof(0xA11CE5EEDull, VW_PARADISE, /*water=*/true));
    EXPECT_TRUE(g.contains(wet, kWaterIdx))
        << "has_water should inline a sea over a submerged ocean column";

    const std::vector<Voxel> dry =
        g.world(ocean, kN, prof(0xA11CE5EEDull, VW_PARADISE, /*water=*/false));
    EXPECT_FALSE(g.contains(dry, kWaterIdx))
        << "with has_water off the plugin must place no water itself";
}

// Sanity: after the profile path runs, the non-profile entry point (used by demo 21
// and VoxelWorldDeterminismTest) resets generation back to the pristine six-biome
// world, independent of any prior profile — so the two demos never interfere.
TEST(NoMansVoxelDeterminism, SetSeedPtrResetsToParadise) {
    Gen g;
    // Leave the plugin in a non-paradise state.
    VwProfile moon = prof(1u, VW_MOON, false);
    voxelworld_set_profile(&moon);

    // The non-profile entry point must restore paradise (many biomes reachable).
    uint64_t seed = 0xA11CE5EEDull;
    voxelworld_set_seed_ptr(&seed);
    std::set<std::string> seen;
    for (int z = -6000; z <= 6000; z += 96)
        for (int x = -6000; x <= 6000; x += 96)
            seen.insert(voxelworld_biome_name(x + 0.5, z + 0.5));
    EXPECT_TRUE(seen.count("Ocean") > 0)
        << "voxelworld_set_seed_ptr must reset to the six-biome world (ocean reachable)";
    EXPECT_GE(seen.size(), 4u);
}
