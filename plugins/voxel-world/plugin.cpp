// voxel-world plugin — the worldgen heart of the "Voxel World" demo (demos/21).
//
// A Minecraft-style overworld with SIX biomes selected deterministically from the
// run's world seed: Ocean, Plains, Forest, Desert, Mountains, and Snowy Tundra.
// Like the mega-demo's overworld plugin this streams a single terminal "terrain"
// layer — bedrock floor, stone bulk, biome-specific subsoil and surface cap — with
// cave/ore feature overlays and a biome-gated decoration pass (trees, cacti, spruce).
//
// Seamlessness (§4): terrain HEIGHT is a continuous blend of low-frequency climate
// fields (continentalness + mountainness + roughness), so chunk borders never seam.
// Biome IDENTITY is a hard quantization of those same continuous fields, used only
// for the surface material and decoration — discrete material seams are fine
// (Minecraft has them). Every lookup is a pure function of world position + seed;
// no rand/time/global mutable state, so evicted chunks regenerate identically.

#include "plugin_api.h"
#include "world/Voxel.h"

#include "voxel_world_profile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();

namespace {

// ── Palette slots (see renderer/Palette.h). Overlap with overworld where it makes
// sense so the shared water plugin's palette-5 blue still applies. ───────────────
constexpr uint8_t kStoneIdx   = 1;   // gray  — bulk underground
constexpr uint8_t kGrassIdx   = 2;   // green — temperate surface cap
constexpr uint8_t kDirtIdx    = 3;   // brown — subsoil
constexpr uint8_t kLeavesIdx  = 4;   // dark green — tree canopy
constexpr uint8_t kWaterIdx   = 5;   // blue  — owned by the water plugin; colored here
constexpr uint8_t kSandIdx    = 6;   // tan   — desert / shoreline / seabed
constexpr uint8_t kSnowIdx    = 7;   // white — tundra / mountain cap
constexpr uint8_t kLogIdx     = 8;   // bark  — tree trunk
constexpr uint8_t kCactusIdx  = 9;   // green — desert cactus
constexpr uint8_t kIronIdx    = 11;  // blue-gray — ore veins
constexpr uint8_t kBedrockIdx = 14;  // dark  — immutable world floor

// ── World shape knobs (world metres, 1 m terminal voxels) ─────────────────────
constexpr int    kSeaLevel   = 15;   // MUST match the water plugin's sea level
constexpr int    kBedrockTop = 1;    // y in [0, kBedrockTop) is immutable bedrock
constexpr int    kDirtDepth  = 3;    // subsoil thickness below the cap
constexpr int    kSnowLine   = 42;   // mountain height above which caps turn to snow

// ── Biomes ────────────────────────────────────────────────────────────────────
enum class Biome { Ocean, Plains, Forest, Desert, Mountains, SnowyTundra };

MaterialProperties material(uint8_t palette, float density, float strength, float hardness,
                            float porosity = 0.0f) {
    MaterialProperties m;
    m.density             = density;
    m.structural_strength = strength;
    m.hardness            = hardness;
    m.porosity            = porosity;
    m.palette_index       = palette;
    return m;
}

// ── Inline value noise (plugin-local: a plugin links zero engine symbols, so it
// cannot call src/world/Noise.cpp — ARCHITECTURE §12). Same lattice/hash as the
// overworld/recipe-world plugins, so behaviour matches the rest of the engine. ──
uint64_t splitmix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
double lattice(int64_t ix, int64_t iy, int64_t iz, uint64_t seed) {
    uint64_t h = seed;
    h ^= static_cast<uint64_t>(ix) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(iy) * 0xC2B2AE3D27D4EB4Full;
    h ^= static_cast<uint64_t>(iz) * 0x165667B19E3779F9ull;
    h = splitmix(h);
    return static_cast<double>(h >> 40) / static_cast<double>(1ull << 24);  // [0,1)
}
double smoother(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
double lerp(double a, double b, double t) { return a + (b - a) * t; }
double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
// Hermite smoothstep between edge0 and edge1.
double smoothstep(double e0, double e1, double x) {
    if (e1 <= e0) return x < e0 ? 0.0 : 1.0;
    const double t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

double value3(double x, double y, double z, uint64_t seed, double frequency) {
    const double fx = x * frequency, fy = y * frequency, fz = z * frequency;
    const int64_t ix = static_cast<int64_t>(std::floor(fx));
    const int64_t iy = static_cast<int64_t>(std::floor(fy));
    const int64_t iz = static_cast<int64_t>(std::floor(fz));
    const double tx = smoother(fx - ix), ty = smoother(fy - iy), tz = smoother(fz - iz);
    const double c000 = lattice(ix,     iy,     iz,     seed);
    const double c100 = lattice(ix + 1, iy,     iz,     seed);
    const double c010 = lattice(ix,     iy + 1, iz,     seed);
    const double c110 = lattice(ix + 1, iy + 1, iz,     seed);
    const double c001 = lattice(ix,     iy,     iz + 1, seed);
    const double c101 = lattice(ix + 1, iy,     iz + 1, seed);
    const double c011 = lattice(ix,     iy + 1, iz + 1, seed);
    const double c111 = lattice(ix + 1, iy + 1, iz + 1, seed);
    const double x00 = lerp(c000, c100, tx);
    const double x10 = lerp(c010, c110, tx);
    const double x01 = lerp(c001, c101, tx);
    const double x11 = lerp(c011, c111, tx);
    return lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);  // [0,1)
}

// Fractal value noise over the (x,z) plane in [0,1), with a caller-set base
// frequency and octave count. Climate fields use a low base frequency (large
// features); the surface-roughness field uses a higher one.
double fbm2_base(double x, double z, uint64_t seed, double baseFreq, int octaves) {
    double sum = 0.0, norm = 0.0, amp = 0.5, freq = baseFreq;
    for (int o = 0; o < octaves; ++o) {
        sum  += amp * value3(x, 0.0, z, seed + static_cast<uint64_t>(o) * 1013u, freq);
        norm += amp;
        amp  *= 0.5;
        freq *= 2.0;
    }
    return sum / norm;  // [0,1)
}

// ── Toroidal wrap seam blend (demo, see demos/21-voxel-world) ─────────────────
// When the host sets a wrap period (voxelworld_set_wrap), the world is a torus of
// g_wrapPeriodM metres in x and z. Rather than periodicise the noise itself, every
// surface/climate field — which all route through fbm2 — is blended across a band
// of width g_wrapBandM at the high (+H) edge toward the wrapped-around terrain, so
// the seam matches in value AND slope (a smoothstep weight has zero derivative at
// both band ends). The interior is byte-for-byte the native field; non-wrapping
// worlds (period 0) skip all of this. Caves (3D value noise) are not blended — an
// underground carve rarely meets the surface seam.
double g_wrapPeriodM = 0.0;   // torus period in metres per axis (0 = not wrapping)
double g_wrapBandM   = 0.0;   // seam blend band width in metres

// ── Active world profile (M19, "No Man's Voxel") ──────────────────────────────
// The multi-world demo (demos/22) sets these via voxelworld_set_profile before
// streaming each world's chunk batch; single-threaded synchronous generation makes
// that safe. Default = demo 21's world (six biomes, no inlined water), and
// voxelworld_set_seed_ptr resets them here, so the un-profiled path is unchanged.
int      g_biomeMode = VW_PARADISE;  // a VwBiomeMode; selects the biome set
bool     g_hasWater  = false;        // true ⇒ inline a flat sea (demo 22's paradise)
uint64_t g_profileSeed = 0;          // backing store for the active profile's seed

// Smoothstep 0->1 across the blend band [H-band, H]; 0 below it. Quintic (the same
// zero-2nd-derivative curve as the noise interpolant) so the blend leaves no crease.
double seamWeight(double c, double H, double band) {
    const double t = (c - (H - band)) / band;
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return smoother(t);
}

double fbm2(double x, double z, uint64_t seed, double baseFreq, int octaves) {
    if (g_wrapPeriodM <= 0.0 || g_wrapBandM <= 0.0)
        return fbm2_base(x, z, seed, baseFreq, octaves);
    const double P = g_wrapPeriodM, H = 0.5 * P, W = g_wrapBandM;
    const double wx = seamWeight(x, H, W);
    const double wz = seamWeight(z, H, W);
    if (wx == 0.0 && wz == 0.0)
        return fbm2_base(x, z, seed, baseFreq, octaves);  // pristine interior
    // Bilinear blend toward the far edge (x-P / z-P) and, in the corner, the far
    // corner (x-P, z-P). At x=+H,wx=1 the value is exactly the native -H edge.
    const double f00 = fbm2_base(x,     z,     seed, baseFreq, octaves);
    const double f10 = fbm2_base(x - P, z,     seed, baseFreq, octaves);
    const double f01 = fbm2_base(x,     z - P, seed, baseFreq, octaves);
    const double f11 = fbm2_base(x - P, z - P, seed, baseFreq, octaves);
    return lerp(lerp(f00, f10, wx), lerp(f01, f11, wx), wz);
}

// ── Climate fields (pure fns of world x,z + seed; continuous & low-frequency) ──
// Decorrelated per field via voxel_seed_mix so temperature/humidity/etc. move
// independently. Base frequencies chosen so biomes span hundreds of metres.
double continentalness(double wx, double wz, uint64_t seed) {
    return fbm2(wx, wz, voxel_seed_mix(seed, 0xC0417u), 1.0 / 640.0, 3);
}
double mountainness(double wx, double wz, uint64_t seed) {
    return fbm2(wx, wz, voxel_seed_mix(seed, 0x3070u), 1.0 / 480.0, 3);
}
double temperature(double wx, double wz, uint64_t seed) {
    return fbm2(wx, wz, voxel_seed_mix(seed, 0x7E37u), 1.0 / 512.0, 2);
}
double humidity(double wx, double wz, uint64_t seed) {
    return fbm2(wx, wz, voxel_seed_mix(seed, 0x40D17u), 1.0 / 448.0, 2);
}

// "How much land" a column is: 0 in deep ocean, 1 well inland. Drives the smooth
// coast-to-inland height ramp (and keeps ocean biome terrain below sea level).
double landFactor(double cont) { return smoothstep(0.38, 0.52, cont); }

// Mountain lift in metres, ramped by mountainness and gated to land only.
double mountainLift(double mtn, double land) {
    return smoothstep(0.55, 0.86, mtn) * land * 58.0;
}

// Continuous surface height (integer world-Y of the topmost solid voxel) at (x,z).
// Shared by the terrain fill, the cave carve, and the decoration pass so they all
// agree on the surface.
int surfaceHeight(double wx, double wz, uint64_t seed) {
    if (g_biomeMode == VW_MOON) {
        // A dead moon: gently rolling grey regolith pocked with impact craters.
        // No ocean/mountain-lift shaping — just a low-amplitude base plus bowl
        // depressions where a mid-frequency field peaks. All from the shared
        // value-noise helpers (no new materials/textures), so it stays seamless
        // and deterministic like every other field.
        const double base = fbm2(wx, wz, voxel_seed_mix(seed, 0x3070u), 1.0 / 220.0, 3);
        double h = kSeaLevel + (base - 0.5) * 22.0;               // ~ ±11 around sea level
        const double c = fbm2(wx, wz, voxel_seed_mix(seed, 0xC7A7E1u), 1.0 / 64.0, 2);
        h -= smoothstep(0.60, 0.90, c) * 14.0;                    // carve crater bowls
        const double rough = fbm2(wx, wz, voxel_seed_mix(seed, 0x2076u), 1.0 / 40.0, 4) - 0.5;
        h += rough * 2.5;                                         // fine rubble
        return static_cast<int>(std::llround(h));
    }
    const double cont = continentalness(wx, wz, seed);
    const double mtn  = mountainness(wx, wz, seed);
    const double land = landFactor(cont);
    const double oceanFloor = kSeaLevel - 8.0;   // ~7
    const double plainsTop  = kSeaLevel + 5.0;   // ~20
    double h = lerp(oceanFloor, plainsTop, land) + mountainLift(mtn, land);
    // Small-scale roughness — flat seas, rolling land, rugged peaks.
    const double rough = fbm2(wx, wz, voxel_seed_mix(seed, 0x2076u), 1.0 / 40.0, 4) - 0.5;
    h += rough * 2.0 * (2.0 + land * 4.0);
    return static_cast<int>(std::llround(h));
}

// Discrete biome for a column (pure fn of x,z + seed). Uses the same continuous
// fields as the height so the two stay consistent (ocean columns sit below sea
// level, mountain columns are the high ones).
Biome biomeAt(double wx, double wz, uint64_t seed) {
    // The moon has no biome concept (handled wholesale in the generator); report a
    // neutral bare biome so any stray decoration query gates itself out.
    if (g_biomeMode == VW_MOON) return Biome::Mountains;

    const double cont = continentalness(wx, wz, seed);
    const double land = landFactor(cont);
    const double mtn  = mountainness(wx, wz, seed);

    if (g_biomeMode == VW_PARADISE) {
        if (land < 0.5) return Biome::Ocean;
        if (mountainLift(mtn, land) > 16.0) return Biome::Mountains;
        const double temp = temperature(wx, wz, seed);
        const double hum  = humidity(wx, wz, seed);
        if (temp < 0.36)               return Biome::SnowyTundra;
        if (temp > 0.64 && hum < 0.42) return Biome::Desert;
        if (hum  > 0.54)               return Biome::Forest;
        return Biome::Plains;
    }

    // Simpler worlds (demo 22): one dominant biome plus mountain ranges where the
    // mountain field peaks — no ocean (these worlds carry no water), so a low
    // continentalness basin is just dry dominant-biome ground.
    if (mountainLift(mtn, land) > 16.0) return Biome::Mountains;
    return (g_biomeMode == VW_SNOWY) ? Biome::SnowyTundra : Biome::Desert;
}

const char* biomeName(Biome b) {
    switch (b) {
        case Biome::Ocean:       return "Ocean";
        case Biome::Plains:      return "Plains";
        case Biome::Forest:      return "Forest";
        case Biome::Desert:      return "Desert";
        case Biome::Mountains:   return "Mountains";
        case Biome::SnowyTundra: return "Snowy Tundra";
    }
    return "?";
}

uint64_t seedFrom(void* user_data) {
    return user_data ? *static_cast<const uint64_t*>(user_data) : 0x9E3779B97F4A7C15ull;
}

// ── Layer generator: biome heightmap (terminal "terrain" layer) ───────────────
void terrain_generator(WorldCoord chunk_origin, int grid_size, Voxel* out, void* user_data) {
    const uint64_t seed = seedFrom(user_data);
    const MaterialProperties grass   = material(kGrassIdx,   1500.0f, 0.70f, 0.30f);
    const MaterialProperties dirt    = material(kDirtIdx,    1300.0f, 0.65f, 0.25f);
    const MaterialProperties stone   = material(kStoneIdx,   2700.0f, 0.85f, 0.70f);
    const MaterialProperties sand    = material(kSandIdx,    1600.0f, 0.55f, 0.20f, 0.30f);
    const MaterialProperties snow    = material(kSnowIdx,     900.0f, 0.40f, 0.15f);
    const MaterialProperties bedrock = material(kBedrockIdx, 4000.0f, 1.00f, 1.00f);
    // Matches the registered "water" material (porosity 1); used only when a
    // profile sets has_water (demo 22's paradise inlines its own sea).
    const MaterialProperties water   = material(kWaterIdx,   1000.0f, 0.00f, 0.00f, 1.0f);

    const int64_t baseX = static_cast<int64_t>(std::llround(chunk_origin.value.x));
    const int64_t baseY = static_cast<int64_t>(std::llround(chunk_origin.value.y));
    const int64_t baseZ = static_cast<int64_t>(std::llround(chunk_origin.value.z));

    for (int z = 0; z < grid_size; ++z)
        for (int x = 0; x < grid_size; ++x) {
            const double wx = static_cast<double>(baseX + x) + 0.5;
            const double wz = static_cast<double>(baseZ + z) + 0.5;
            const int    h  = surfaceHeight(wx, wz, seed);
            const Biome  b  = biomeAt(wx, wz, seed);

            // Choose the surface cap + subsoil. The moon is bare grey regolith
            // (stone all the way up), so it skips the biome cap logic entirely.
            MaterialProperties cap = grass, sub = dirt;
            if (g_biomeMode == VW_MOON) {
                cap = stone; sub = stone;
            } else {
                const bool shore = h <= kSeaLevel + 1;   // beaches / lake edges
                switch (b) {
                    case Biome::Ocean:  cap = sand;  sub = sand; break;
                    case Biome::Desert: cap = sand;  sub = sand; break;
                    case Biome::SnowyTundra: cap = snow; sub = dirt; break;
                    case Biome::Mountains:
                        cap = (h >= kSnowLine) ? snow : stone;
                        sub = stone; break;
                    case Biome::Plains:
                    case Biome::Forest:
                        cap = grass; sub = dirt; break;
                }
                // A wet shoreline is sandy regardless of the inland biome (except the
                // already-sandy desert/ocean and the snow-capped tundra).
                if (shore && b != Biome::Ocean && b != Biome::Desert && b != Biome::SnowyTundra) {
                    cap = sand; sub = sand;
                }
            }

            for (int y = 0; y < grid_size; ++y) {
                const int64_t wy = baseY + y;
                Voxel& v = out[x + grid_size * (y + grid_size * z)];
                if (wy < kBedrockTop)           v = Voxel{bedrock};
                else if (wy > h)                v = Voxel::empty();
                else if (wy == h)               v = Voxel{cap};
                else if (wy >= h - kDirtDepth)  v = Voxel{sub};
                else                            v = Voxel{stone};
            }

            // Inlined flat sea (M19): when a profile requests water, flood empty
            // cells from just above the surface up to sea level. The multi-world
            // demo uses this instead of loading the separate water plugin; demo 21
            // (g_hasWater == false) is unaffected and still floods via that plugin.
            if (g_hasWater && h < kSeaLevel) {
                for (int y = 0; y < grid_size; ++y) {
                    const int64_t wy = baseY + y;
                    if (wy <= h || wy > kSeaLevel) continue;
                    Voxel& v = out[x + grid_size * (y + grid_size * z)];
                    if (v.isEmpty()) v = Voxel{water};
                }
            }
        }
}

// ── Feature generator: caves ─────────────────────────────────────────────────
// Carves connected voids from the stony bulk where 3D value noise falls below a
// depth-ramped density. (Same model as the overworld plugin.)
void cave_feature(WorldCoord origin, double vs, int n, Voxel* inout,
                  const RecipeParam* params, size_t count, uint64_t seed, void*) {
    constexpr int kSurfaceMargin = 3;
    const double surfaceDensity = recipe_param_num(params, count, "cave_density_surface", 0.06);
    const double depthDensity   = recipe_param_num(params, count, "cave_density_depth", 0.12);
    const double scale          = recipe_param_num(params, count, "scale", 9.0);
    const double freq = (scale > 0.0) ? (1.0 / scale) : (1.0 / 9.0);
    const uint64_t caveSeed = voxel_seed_mix(seed, 0xCA7E5u);

    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const double wx = origin.value.x + (x + 0.5) * vs;
            const double wz = origin.value.z + (z + 0.5) * vs;
            const int surf = surfaceHeight(wx, wz, seed);
            for (int y = 0; y < n; ++y) {
                Voxel& v = inout[x + n * (y + n * z)];
                if (v.isEmpty()) continue;
                if (v.material.palette_index == kBedrockIdx) continue;
                const double wy = origin.value.y + (y + 0.5) * vs;
                const int iy = static_cast<int>(std::floor(wy));
                if (iy > surf - kSurfaceMargin || iy < kBedrockTop) continue;
                const double depthFrac =
                    std::clamp(static_cast<double>(surf - iy) / 22.0, 0.0, 1.0);
                const double density =
                    std::clamp(surfaceDensity + (depthDensity - surfaceDensity) * depthFrac, 0.0, 1.0);
                if (value3(wx, wy, wz, caveSeed, freq) < density)
                    v = Voxel::empty();
            }
        }
}

// ── Feature generator: ore veins ─────────────────────────────────────────────
void ore_feature(WorldCoord origin, double vs, int n, Voxel* inout,
                 const RecipeParam* params, size_t count, uint64_t seed, void*) {
    if (g_biomeMode == VW_MOON) return;   // dead moon: no ore veins
    const double richness = std::clamp(recipe_param_num(params, count, "ore_richness", 0.14), 0.0, 1.0);
    const double scale    = recipe_param_num(params, count, "scale", 6.0);
    const double freq = (scale > 0.0) ? (1.0 / scale) : (1.0 / 6.0);
    const uint64_t oreSeed = voxel_seed_mix(seed, 0x0BEull);
    const MaterialProperties ore = material(kIronIdx, 5200.0f, 0.9f, 0.85f);

    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                Voxel& v = inout[x + n * (y + n * z)];
                if (v.material.palette_index != kStoneIdx) continue;
                const double wx = origin.value.x + (x + 0.5) * vs;
                const double wy = origin.value.y + (y + 0.5) * vs;
                const double wz = origin.value.z + (z + 0.5) * vs;
                if (value3(wx, wy, wz, oreSeed, freq) < richness)
                    v.material = ore;
            }
}

// ── Feature generator: biome-gated decoration (trees / spruce / cacti) ────────
// Deterministic per-column, exactly the trees-plugin pattern: hash the world
// column, spawn density depends on the biome under the column. Trunks/canopy that
// cross the chunk grid bound are clipped (per-chunk behaviour, like every feature).
void decoration_feature(WorldCoord origin, double /*voxel_size_m*/, int n, Voxel* inout,
                        const RecipeParam* params, size_t count, uint64_t seed, void*) {
    (void)params; (void)count;
    if (g_biomeMode == VW_MOON) return;   // dead moon: no trees or cacti
    const uint64_t decoSeed = voxel_seed_mix(seed, 0xDEC0u);
    const MaterialProperties log    = material(kLogIdx,    600.0f, 0.60f, 0.35f);
    const MaterialProperties leaf   = material(kLeavesIdx, 200.0f, 0.05f, 0.10f, 0.40f);
    const MaterialProperties cactus = material(kCactusIdx, 500.0f, 0.30f, 0.20f, 0.20f);

    auto at = [&](int x, int y, int z) -> Voxel& { return inout[x + n * (y + n * z)]; };

    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            // Topmost solid, non-water voxel with air directly above.
            int gy = -1;
            for (int y = n - 2; y >= 1; --y) {
                const Voxel& s = at(x, y, z);
                if (!s.isEmpty() && s.material.palette_index != kWaterIdx &&
                    at(x, y + 1, z).isEmpty()) { gy = y; break; }
            }
            if (gy < 0) continue;
            const uint8_t cap = at(x, gy, z).material.palette_index;

            const int64_t wx = static_cast<int64_t>(std::llround(origin.value.x)) + x;
            const int64_t wz = static_cast<int64_t>(std::llround(origin.value.z)) + z;
            const Biome b = biomeAt(static_cast<double>(wx) + 0.5,
                                    static_cast<double>(wz) + 0.5, seed);

            double density = 0.0;
            bool isCactus = false, isTree = false;
            switch (b) {
                case Biome::Forest:      density = 0.060; isTree = true; break;
                case Biome::Plains:      density = 0.006; isTree = true; break;
                case Biome::SnowyTundra: density = 0.005; isTree = true; break;
                case Biome::Desert:      density = 0.010; isCactus = true; break;
                default: continue;  // Ocean / Mountains: bare
            }
            // Trees only take root on a grassy cap; cacti only on sand.
            if (isTree   && cap != kGrassIdx) continue;
            if (isCactus && cap != kSandIdx)  continue;

            uint64_t st = voxel_seed_mix(decoSeed,
                              voxel_seed_mix(static_cast<uint64_t>(wx) * 0x1f1f1f1fu + 1u,
                                             static_cast<uint64_t>(wz) * 0x9e3779b9u + 7u));
            if (voxel_rng_norm(&st) >= density) continue;

            if (isCactus) {
                const int hgt = 1 + static_cast<int>(voxel_rng_norm(&st) * 3.0f);  // 1..3
                for (int t = 1; t <= hgt; ++t) {
                    const int y = gy + t;
                    if (y >= n) break;
                    at(x, y, z) = Voxel{cactus};
                }
                continue;
            }

            // Tree: a straight trunk with a small rounded canopy (leaves into air
            // only). Snowy tundra grows a narrower, slightly taller spruce.
            const bool spruce = (b == Biome::SnowyTundra);
            const int trunkH = (spruce ? 5 : 4) + static_cast<int>(voxel_rng_norm(&st) * 3.0f);
            for (int t = 1; t <= trunkH; ++t) {
                const int y = gy + t;
                if (y >= n) break;
                at(x, y, z) = Voxel{log};
            }
            const int topY = gy + trunkH;
            for (int dy = -2; dy <= 1; ++dy) {
                const int y = topY + dy;
                if (y <= gy || y >= n) continue;
                int r = (dy >= 1) ? 1 : 2;
                if (spruce) r = (dy >= 0) ? 1 : 2;  // tighter conifer silhouette
                for (int dz = -r; dz <= r; ++dz)
                    for (int dx = -r; dx <= r; ++dx) {
                        if (dx == 0 && dz == 0 && dy < 1) continue;   // keep the trunk core
                        if (dx * dx + dz * dz > r * r + 1) continue;  // round the corners
                        const int cx = x + dx, cz = z + dz;
                        if (cx < 0 || cx >= n || cz < 0 || cz >= n) continue;
                        Voxel& c = at(cx, y, cz);
                        if (c.isEmpty()) c = Voxel{leaf};
                    }
            }
        }
}

void bindFaces(PluginContext* ctx, uint8_t palette,
               const char* top, const char* bottom, const char* side) {
    ctx->set_material_faces(ctx, palette, top, bottom, side, 1.0f);
}

// World seed pointer — stored at init so the exported biome query (used by the
// demo HUD) and the registered noise can read the run's seed.
uint64_t* g_seedPtr = nullptr;

// Registered heightmap noise (solid >= 0.5): lets tooling/tests query the surface.
float voxelworld_height_noise(WorldCoord pos, uint64_t /*seed*/,
                              const RecipeParam* params, size_t count, void* /*user_data*/) {
    const uint64_t worldSeed = static_cast<uint64_t>(
        recipe_param_num(params, count, "world_seed",
                         static_cast<double>(0x9E3779B97F4A7C15ull)));
    const int h = surfaceHeight(pos.value.x, pos.value.z, worldSeed);
    return (pos.value.y <= static_cast<double>(h) + 0.5) ? 0.9f : 0.1f;
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxelworld_plugin_init(PluginContext* ctx) {
    // Materials (M8).
    ctx->register_material(ctx, "grass",    material(kGrassIdx,   1500.0f, 0.70f, 0.30f));
    ctx->register_material(ctx, "dirt",     material(kDirtIdx,    1300.0f, 0.65f, 0.25f));
    ctx->register_material(ctx, "stone",    material(kStoneIdx,   2700.0f, 0.85f, 0.70f));
    ctx->register_material(ctx, "sand",     material(kSandIdx,    1600.0f, 0.55f, 0.20f, 0.30f));
    ctx->register_material(ctx, "snow",     material(kSnowIdx,     900.0f, 0.40f, 0.15f));
    ctx->register_material(ctx, "log",      material(kLogIdx,      600.0f, 0.60f, 0.35f));
    ctx->register_material(ctx, "leaves",   material(kLeavesIdx,   200.0f, 0.05f, 0.10f, 0.40f));
    ctx->register_material(ctx, "cactus",   material(kCactusIdx,   500.0f, 0.30f, 0.20f, 0.20f));
    ctx->register_material(ctx, "iron-ore", material(kIronIdx,    5200.0f, 0.90f, 0.85f));
    ctx->register_material(ctx, "bedrock",  material(kBedrockIdx, 4000.0f, 1.00f, 1.00f));
    ctx->register_material(ctx, "water",
                           material(kWaterIdx, 1000.0f, 0.0f, 0.0f, /*porosity=*/1.0f));

    // Palette colours (ABGR 0xAABBGGRR).
    ctx->set_palette_color(ctx, kStoneIdx,   0xff7a7a7a);
    ctx->set_palette_color(ctx, kGrassIdx,   0xff3fa83f);
    ctx->set_palette_color(ctx, kDirtIdx,    0xff3f5a8b);
    ctx->set_palette_color(ctx, kLeavesIdx,  0xff2f7a3a);
    ctx->set_palette_color(ctx, kWaterIdx,   0xffd08a3a);
    ctx->set_palette_color(ctx, kSandIdx,    0xff8fd6e6);
    ctx->set_palette_color(ctx, kSnowIdx,    0xfff8f8ff);
    ctx->set_palette_color(ctx, kLogIdx,     0xff2f4a6b);
    ctx->set_palette_color(ctx, kCactusIdx,  0xff3c823c);
    ctx->set_palette_color(ctx, kIronIdx,    0xff9a9aa8);
    ctx->set_palette_color(ctx, kBedrockIdx, 0xff202028);

    // M15 textures — the demo synthesises these PNG tiles at the paths below.
    const char* kTexDir = "assets/textures/voxelworld/";
    const char* kTiles[] = {"grass_top", "grass_side", "dirt", "stone", "sand",
                            "snow", "log_top", "log_side", "leaves", "cactus"};
    for (const char* id : kTiles) {
        const std::string path = std::string(kTexDir) + id + ".png";
        ctx->register_texture(ctx, id, path.c_str());
    }
    bindFaces(ctx, kGrassIdx,  "grass_top", "dirt",     "grass_side");
    bindFaces(ctx, kDirtIdx,   "dirt",      "dirt",     "dirt");
    bindFaces(ctx, kStoneIdx,  "stone",     "stone",    "stone");
    bindFaces(ctx, kSandIdx,   "sand",      "sand",     "sand");
    bindFaces(ctx, kSnowIdx,   "snow",      "dirt",     "snow");
    bindFaces(ctx, kLogIdx,    "log_top",   "log_top",  "log_side");
    bindFaces(ctx, kLeavesIdx, "leaves",    "leaves",   "leaves");
    bindFaces(ctx, kCactusIdx, "cactus",    "cactus",   "cactus");

    ctx->register_noise(ctx, "voxelworld_height", voxelworld_height_noise, g_seedPtr);

    // Terminal "terrain" layer generator + feature overlays. The demo applies the
    // feature generators in registration order (caves → ore → decoration), after
    // the water plugin floods the sea, so trees/cacti root on the final surface.
    ctx->register_layer_generator(ctx, "terrain", terrain_generator, nullptr);
    ctx->register_feature_generator(ctx, "cave",       cave_feature,       nullptr);
    ctx->register_feature_generator(ctx, "ore",        ore_feature,        nullptr);
    ctx->register_feature_generator(ctx, "decoration", decoration_feature, nullptr);
    return 0;
}

// Set the world seed pointer before init (host or tests) so the biome query and
// the registered noise use the run's seed.
VOXEL_PLUGIN_EXPORT void voxelworld_set_seed_ptr(uint64_t* ptr) {
    g_seedPtr = ptr;
    // Reset generation to the demo-21 defaults (pristine six-biome world, no
    // inlined water, no wrap) so this non-profile entry point is independent of any
    // prior voxelworld_set_profile call. Demo 21 calls set_wrap AFTER this, so the
    // wrap reset here does not clobber a wrapping world; the multi-world demo uses
    // voxelworld_set_profile instead of this entry point. Keeps tests order-free.
    g_biomeMode   = VW_PARADISE;
    g_hasWater    = false;
    g_wrapPeriodM = 0.0;
    g_wrapBandM   = 0.0;
}

// Activate a world profile (M19). The demo sets this before streaming each world's
// chunk batch; the plugin reads g_biomeMode/g_hasWater/wrap in its generator and
// feature passes. The seed is stored internally and pointed at by g_seedPtr so the
// HUD biome query uses the active world's seed (the demo also passes the same seed
// value to the generator via user_data). Declared in voxel_world_profile.h as plain
// extern "C" (it is only ever called compiled-in, never across a DLL boundary), so
// the definition must match that linkage — no dllexport here.
extern "C" void voxelworld_set_profile(const VwProfile* p) {
    if (!p) return;
    g_profileSeed = p->seed;
    g_seedPtr     = &g_profileSeed;
    g_biomeMode   = p->biome_mode;
    g_hasWater    = p->has_water != 0;
    g_wrapPeriodM = p->period_m;
    g_wrapBandM   = p->band_m;
}

// Enable/disable the toroidal wrap seam blend. period_m is the world period per
// horizontal axis (0 disables — the world generates as normal); band_m is the
// width of the transitional swath at the +period/2 edge. Set before generation.
VOXEL_PLUGIN_EXPORT void voxelworld_set_wrap(double period_m, double band_m) {
    g_wrapPeriodM = period_m;
    g_wrapBandM   = band_m;
}

// Biome name under a world column — the demo HUD reads this for the current biome.
// Uses the seed set via voxelworld_set_seed_ptr (falls back to a fixed constant).
VOXEL_PLUGIN_EXPORT const char* voxelworld_biome_name(double wx, double wz) {
    const uint64_t seed = g_seedPtr ? *g_seedPtr : 0x9E3779B97F4A7C15ull;
    return biomeName(biomeAt(wx, wz, seed));
}

#ifndef VOXELWORLD_COMPILED_IN
VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    return voxelworld_plugin_init(ctx);
}
#endif
