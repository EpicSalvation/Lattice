// Demo — No Man's Voxel: fly between several worlds in one session, no loading screen.
//
// A follow-on to the Voxel World demo (demos/21), riffing on No Man's Sky. You
// spawn on a PARADISE world — demo 21's full six-biome world with an ocean — then
// fly UP and out past its local bounds into open space, cross the void, and DESCEND
// onto other, simpler nearby worlds: a warm Desert, a Snowy tundra, and a dead grey
// Moon. Everything happens in one continuous session; there is no jump-cut.
//
// ── The architecture: scene space vs. world-local space ──────────────────────
// The camera flies in one continuous double-precision "scene" space. Each world
// owns a SCENE OFFSET (its XZ placement, with empty void gaps between footprints)
// and its own toroidal wrap (WorldWrap.h). While the camera is horizontally inside
// a world's footprint, its local position is `sceneXZ − offset` folded onto that
// world's torus, and the world streams around it; its chunks render at
// `offset + torus.nearestOriginM(localOrigin, localCam)` — the same periodic-image
// trick demo 21 uses, shifted into scene space. Footprints never overlap, so at
// most ONE world streams at a time; between footprints nothing streams — that is
// "space". The vertical axis is SHARED (every world's ground sits in the same
// ~0..80 m band; only XZ is offset), so the camera stays Y-up throughout and no
// surface-normal camera basis (setCameraUp) is needed.
//
// Leaving a world: you fly UP. Below a world's atmosphere top the torus wrap is
// engaged, so flying horizontally loops you back around the planet (like demo 21's
// --wrap). Climb above the atmosphere top and the wrap releases — the sky has by
// then faded to the space backdrop (setSky) and the fog has thinned to nothing
// (setFog) — and you are free to drift across the void to another footprint and
// descend. Number keys 1..4 teleport-and-land on each world's centre.
//
// Wayfinding: out in the space view (high above a world, in the void, or on the
// airless moon) the far clip opens up and the other worlds appear as coarse
// **proxy planets** at their true scene positions — pick one, fly to it, and as you
// enter its footprint the proxy hands off to real streamed terrain. A camera-centred
// **starfield** shell fills the void behind them. Both are ordinary chunk meshes
// (ChunkMesh + renderChunk) — no new renderer seam; a crisp/textured star layer
// remains the recorded M17 follow-on, so these stars are deliberately blocky.
//
// ── What each world is ───────────────────────────────────────────────────────
// All four are the SAME voxel-world worldgen plugin (demos/21), driven per-world by
// a VwProfile (voxel_world_profile.h): a distinct seed, torus period, biome set,
// and whether it carries water. Paradise is the six-biome world (ocean inlined);
// Desert and Snowy are single-dominant-biome worlds with mountain ranges and no
// water; the Moon is bare grey regolith with impact craters and no water, trees, or
// ore. This is the M17 renderer groundwork's forcing function: the procedural-sky
// seam (Renderer::setSky / procedural_sky.h), the distance-fog seam
// (Renderer::setFog), and the reusable toroidal wrap (demos/common/WorldWrap.h).
//
// Controls: WASD move, mouse look, Space/Shift fly up/down, hold Left-Ctrl to fly
// FAST, 1..4 teleport to a world, F toggles the cursor, ESC quits. Pass a seed as
// the first argument (`22-no-mans-voxel 12345`); omit it for a deterministic default.

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "plugin_api.h"            // voxel_seed_mix, PluginContext
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/LODManager.h"
#include "renderer/TextureManager.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/World.h"

#include "../common/WorldWrap.h"
#include "procedural_sky.h"        // psky::daySky/spaceSky/blend (header-only)
#include "voxel_world_profile.h"   // VwProfile / voxelworld_set_profile

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The voxel-world worldgen plugin is COMPILED INTO this demo (guarded by
// VOXELWORLD_COMPILED_IN) so we can call its custom entry points directly — the
// per-world profile and the biome HUD query. No water plugin: paradise inlines its
// own sea (has_water in the profile).
extern "C" int         voxelworld_plugin_init(PluginContext* ctx);
extern "C" void        voxelworld_set_profile(const VwProfile* p);
extern "C" const char* voxelworld_biome_name(double wx, double wz);

namespace {
constexpr char     kLogCat[]      = "demo22";
constexpr int      kLoadsPerFrame = 4;
constexpr float    kCruiseSpeed   = 30.0f;   // m/s — normal exploration
constexpr float    kFastSpeed     = 160.0f;  // m/s — hold Left-Ctrl to cover ground
constexpr float    kMouseSens     = 0.002f;
constexpr uint64_t kDefaultSeed   = 0xA11CE5EEDull;  // demo 21's default seed
constexpr int      kMaxChunkYBand = 2;       // terrain spans ~0..80 m; a 3-band box covers it

// Below a world's atmosphere top the torus wrap is engaged (fly-around); above it,
// the wrap releases so you can leave. The sky/fog fade over [atmoLow, atmoHigh].
constexpr double   kAtmoLowM  = 90.0;
constexpr double   kAtmoHighM = 240.0;
// A world is "active" (streaming/rendered) when the camera XZ is within its
// footprint grown by this margin, so chunks appear just before you cross in.
constexpr double   kFootprintMarginM = 48.0;
// The clear color out in open space (matches the dark space sky).
const glm::vec3    kSpaceColor{0.01f, 0.01f, 0.03f};

// ── Space view: far clip, proxy planets, and the starfield ───────────────────
// On an atmospheric surface a short far clip keeps depth precision tight and fog
// hides the streaming edge; once you climb out (or step onto the airless moon, or
// drift into the void) we switch to a much larger clip so the distant sister worlds
// and the starfield come into range.
constexpr float    kSurfaceFarClipM  = 500.0f;
constexpr float    kSpaceFarClipM    = 3200.0f;   // reaches every world in the layout
constexpr double   kSpaceViewAltFrac = 0.35;      // sky-fade fraction at which space view starts
// Proxy "planet": a coarse voxel ball drawn at a world's scene position so it can
// be seen and flown toward from afar; it hands off to real streamed terrain once
// you enter the world's footprint (it becomes active and its proxy is skipped).
constexpr int      kProxyGrid    = 22;    // voxels per side of a proxy ball
constexpr double   kProxyVoxelM  = 9.0;   // proxy voxel edge → ~200 m diameter
constexpr double   kProxyCenterYM = 40.0; // scene-Y of a proxy's centre (mid terrain band)
// Starfield: a shell of bright voxels drawn camera-centred (so it sits at infinity
// and can't be flown to), just inside the space far clip so terrain occludes it at
// the horizon. Blocky stars — on theme for a voxel engine (a crisp/textured star
// layer is the recorded M17 renderer follow-on).
constexpr int      kStarGrid    = 128;    // voxels per side of the star-shell grid
constexpr double   kStarRadiusM = 2900.0; // scene radius the shell renders at
constexpr double   kStarProb    = 0.014;  // per-shell-voxel star probability
constexpr uint8_t  kStarPalette = 7;      // snow index — bright white

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

// ───────────────────────── Texture synthesis (PNG) ──────────────────────────
// Self-contained 16px-tile PNG writer, mirrored from demo 21 so the demo has no
// external asset dependency — the voxel-world plugin registers these tile ids by
// path and we synthesise them here on first run. (Moon reuses the stone tile.)
uint32_t hash2(int x, int y, uint32_t s) {
    uint32_t h = s ^ (static_cast<uint32_t>(x) * 374761393u) ^ (static_cast<uint32_t>(y) * 668265263u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
uint32_t crc32_of(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
    return crc;
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x));
}
void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    put32(out, crc32_of(td.data(), td.size()) ^ 0xFFFFFFFFu);
}
bool writePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + w * 4));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint8_t* row = rgba.data() + static_cast<size_t>(y) * w * 4;
        raw.insert(raw.end(), row, row + static_cast<size_t>(w) * 4);
    }
    std::vector<uint8_t> z; z.push_back(0x78); z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size()) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n >= raw.size() ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    put32(z, (b << 16) | a);

    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(w)); put32(ihdr, static_cast<uint32_t>(h));
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);

    std::vector<uint8_t> out = {137, 80, 78, 71, 13, 10, 26, 10};
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return f.good();
}
void makeTile(const std::string& path, uint8_t r, uint8_t g, uint8_t b,
              int jitter, uint32_t seed, int topBandRows = 0,
              uint8_t tr = 0, uint8_t tg = 0, uint8_t tb = 0) {
    constexpr int N = 16;
    std::vector<uint8_t> px(static_cast<size_t>(N) * N * 4);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const int n = static_cast<int>(hash2(x, y, seed) % 32) - 16;
            uint8_t cr = r, cg = g, cb = b;
            if (y < topBandRows) { cr = tr; cg = tg; cb = tb; }
            auto cl = [](int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); };
            const size_t i = (static_cast<size_t>(y) * N + x) * 4;
            px[i + 0] = cl(cr + n * jitter / 16);
            px[i + 1] = cl(cg + n * jitter / 16);
            px[i + 2] = cl(cb + n * jitter / 16);
            px[i + 3] = 255;
        }
    writePng(path, N, N, px);
}
void ensureTextures(const std::string& dir) {
    std::error_code ec; std::filesystem::create_directories(dir, ec);
    auto p = [&](const char* n) { return dir + "/" + n + ".png"; };
    if (std::filesystem::exists(p("grass_top"))) return;
    makeTile(p("grass_top"),  72, 140, 64, 10, 1);
    makeTile(p("grass_side"), 110, 84, 56, 8, 2, 4, 72, 140, 64);
    makeTile(p("dirt"),       110, 84, 56, 8, 3);
    makeTile(p("stone"),      120, 120, 124, 6, 4);
    makeTile(p("sand"),       214, 200, 150, 6, 5);
    makeTile(p("snow"),       236, 240, 248, 4, 6);
    makeTile(p("log_top"),    150, 110, 70, 10, 7);
    makeTile(p("log_side"),   96, 70, 44, 12, 8);
    makeTile(p("leaves"),     58, 120, 52, 14, 9);
    makeTile(p("cactus"),     60, 130, 60, 8, 10);
}

// ── Per-world sky/fog looks (policy literals; the plugin owns none of this) ────
SkyParams desertSky() {
    SkyParams s; s.enabled = true;
    s.zenith  = glm::vec3(0.35f, 0.55f, 0.85f);
    s.horizon = glm::vec3(0.90f, 0.78f, 0.55f);   // warm tan haze
    s.ground  = glm::vec3(0.45f, 0.35f, 0.22f);
    s.horizon_falloff = 0.6f;
    return s;
}
SkyParams snowySky() {
    SkyParams s; s.enabled = true;
    s.zenith  = glm::vec3(0.55f, 0.68f, 0.85f);
    s.horizon = glm::vec3(0.88f, 0.92f, 0.97f);   // near-white
    s.ground  = glm::vec3(0.70f, 0.74f, 0.80f);
    s.horizon_falloff = 0.5f;
    return s;
}
SkyParams moonSky() {
    // Barely any atmosphere: near-space-dark with a faint grey horizon, so the moon
    // reads dead even standing on its surface (atmo blend adds little).
    SkyParams s; s.enabled = true;
    s.zenith  = glm::vec3(0.02f, 0.02f, 0.04f);
    s.horizon = glm::vec3(0.10f, 0.10f, 0.12f);
    s.ground  = glm::vec3(0.05f, 0.05f, 0.06f);
    s.horizon_falloff = 1.0f;
    return s;
}

// A solid voxel of the given palette slot (only palette_index affects how it draws;
// the other material fields are irrelevant for these decorative meshes).
Voxel solidVoxel(uint8_t palette) {
    MaterialProperties m;
    m.density = 1000.0f; m.structural_strength = 1.0f; m.hardness = 1.0f; m.porosity = 0.0f;
    m.palette_index = palette;
    return Voxel{m};
}

// A coarse voxel-ball mesh (a proxy "planet") of the given palette, built once and
// rendered at each world's scene position. Geometry is chunk-local; renderChunk
// applies the world position and the per-draw voxel scale.
ChunkMesh buildBallMesh(uint8_t palette, int n) {
    Chunk c(ChunkCoord{0, 0, 0}, n, WorldCoord(0.0, 0.0, 0.0));
    const double cc = (n - 1) * 0.5;
    const double r  = n * 0.5 - 1.0;
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double dx = x - cc, dy = y - cc, dz = z - cc;
                if (dx * dx + dy * dy + dz * dz <= r * r) c.at(x, y, z) = solidVoxel(palette);
            }
    return ChunkMesh::build(c, 1.0);
}

// A shell of bright voxels scattered on a sphere — the starfield, built once and
// drawn camera-centred. p is the per-shell-voxel lighting probability.
ChunkMesh buildStarMesh(int n, double p) {
    Chunk c(ChunkCoord{0, 0, 0}, n, WorldCoord(0.0, 0.0, 0.0));
    const double cc = (n - 1) * 0.5;
    const double R  = n * 0.5 - 3.0;   // shell radius in voxels (see kStarRadiusM mapping)
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double dx = x - cc, dy = y - cc, dz = z - cc;
                const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (std::abs(d - R) >= 0.9) continue;   // thin shell only
                uint32_t h = hash2(x, y, 0x51EDu);
                h = hash2(static_cast<int>(h & 0xffff), z, 0x2C17u);
                if ((h & 0xffffu) / 65535.0 < p) c.at(x, y, z) = solidVoxel(kStarPalette);
            }
    return ChunkMesh::build(c, 1.0);
}

// One world instance: a genuinely distinct engine World with its own origin,
// toroidal wrap, seed, biome/water profile, and sky/fog look. All worlds share the
// single terminal-terrain LayerConfig (each builds its own World/LODManager from it).
struct WorldInstance {
    std::string        name;
    glm::dvec2         offset;   // scene placement: .x = scene X, .y = scene Z of the local origin
    worldwrap::Torus   torus;
    VwProfile          profile;  // seed, period_m, band_m, biome_mode, has_water
    SkyParams          daySky;
    FogParams          fog;      // atmospheric fog at the surface (color == tint)
    glm::vec3          tint;     // clear color at the surface (fog + far plane match it)
    ChunkMesh          proxyMesh;   // coarse "planet" ball seen from other worlds / space
    World              world;
    LODManager         lod;
    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> meshes;

    explicit WorldInstance(const LayerConfig& cfg) : world(cfg), lod(cfg) {}
};

}  // namespace

int main(int argc, char** argv) {
    // ── Arguments: [seed] ────────────────────────────────────────────────────
    uint64_t baseSeed = kDefaultSeed;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (!arg.empty() && arg[0] != '-') {
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(arg.c_str(), &end, 0);
            if (end && *end == '\0') baseSeed = parsed;
            else Log::warn(kLogCat, "Could not parse seed argument; using the default.");
        }
    }
    Log::info(kLogCat, (std::string("Base world seed: ") + std::to_string(baseSeed)).c_str());

#ifdef VOXEL_REPO_ROOT
    if (!std::filesystem::exists("assets")) {
        std::error_code ec; std::filesystem::current_path(VOXEL_REPO_ROOT, ec);
    }
#endif
    ensureTextures("assets/textures/voxelworld");

    // ── One shared layer config: a terminal "terrain" layer streamed per world ──
    LayerConfig layerConfig = [] {
        try {
            return LayerConfig::loadFromString(R"(
layers:
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 32
    view_distance_chunks: 6
    interactive: true
)");
        } catch (const std::exception& e) {
            Log::error(kLogCat, (std::string("Fatal: layer config error: ") + e.what()).c_str());
            std::exit(1);
        }
    }();

    PluginManager pm;
    Engine engine;
    engine.start();

    platform::Window window(1280, 720, "VoxelEngine — Demo: No Man's Voxel");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(false);
    renderer.setFarClip(500.0f);

    texture::TextureManager textureManager(pm, renderer);

    // Compiled-in worldgen (materials/palette/textures + generator + features).
    pm.wireInPlugin(voxelworld_plugin_init);
    textureManager.rebuild();
    renderer.setAtlas(textureManager.atlas());
    Log::info(kLogCat, (std::string("Texture atlas: ") +
                        std::to_string(textureManager.tileCount()) + " tiles.").c_str());

    LayerGeneratorFn generator = nullptr;
    for (const auto& g : pm.layerGenerators())
        if (g.layer_name == "terrain") generator = g.fn;
    if (!generator) { Log::error(kLogCat, "Fatal: no 'terrain' generator."); return 1; }

    const int    chunkSize       = layerConfig.findLayer("terrain")->chunk_size_voxels;
    const double voxelSize       = layerConfig.findLayer("terrain")->voxel_size_m;
    const double chunkWorldSizeM = voxelSize * chunkSize;
    const int    viewDist        = layerConfig.findLayer("terrain")->view_distance_chunks;
    const int    minPeriod       = 2 * viewDist + 2;   // seam never wraps onto itself

    // ── Build the worlds (paradise + desert + snowy + moon), laid out in a row
    // along +X with void gaps so no two footprints overlap. Each torus period is a
    // whole EVEN number of chunks (>= the streaming diameter). ───────────────────
    auto makeTorus = [&](int periodChunks) {
        periodChunks = std::max(periodChunks, minPeriod);
        periodChunks += (periodChunks & 1);            // force even
        return worldwrap::Torus{periodChunks, chunkWorldSizeM};
    };
    auto seamBand = [&](const worldwrap::Torus& t) {
        return std::max(chunkWorldSizeM, t.periodM() * 0.06);
    };

    struct WorldDesc { const char* name; int periodChunks; VwBiomeMode mode; bool water;
                       SkyParams sky; glm::vec3 tint; float fogDensity; };
    const std::array<WorldDesc, 4> descs = {{
        {"Paradise", 24, VW_PARADISE, true,  psky::daySky(), glm::vec3(0.62f, 0.74f, 0.86f), 1.0f},
        {"Desert",   20, VW_DESERT,   false, desertSky(),    glm::vec3(0.86f, 0.78f, 0.62f), 0.9f},
        {"Snowy",    20, VW_SNOWY,    false, snowySky(),     glm::vec3(0.82f, 0.87f, 0.93f), 1.0f},
        // Moon is airless: no fog, so the starfield and sister worlds stay crisp and
        // the terrain edge meets black space directly.
        {"Moon",     14, VW_MOON,     false, moonSky(),      glm::vec3(0.05f, 0.05f, 0.06f), 0.0f},
    }};

    std::vector<std::unique_ptr<WorldInstance>> worlds;
    worlds.reserve(descs.size());
    double cursorX = 0.0;   // running scene-X placement; centre each footprint + a gap
    const double gapM = 8.0 * chunkWorldSizeM;
    for (size_t i = 0; i < descs.size(); ++i) {
        const WorldDesc& d = descs[i];
        auto w = std::make_unique<WorldInstance>(layerConfig);
        w->name   = d.name;
        w->torus  = makeTorus(d.periodChunks);
        if (i == 0) cursorX = 0.0;                     // paradise centred on the origin
        else        cursorX += w->torus.halfM();       // step in by this world's half-extent
        w->offset = glm::dvec2(cursorX, 0.0);
        cursorX  += w->torus.halfM() + gapM;           // step out past it + the void gap
        w->profile = VwProfile{ voxel_seed_mix(baseSeed, static_cast<uint64_t>(i)),
                                w->torus.periodM(), seamBand(w->torus),
                                static_cast<int>(d.mode), d.water ? 1 : 0 };
        w->daySky = d.sky;
        w->tint   = d.tint;
        w->fog    = FogParams{ d.tint, 200.0f, 460.0f, d.fogDensity };
        w->lod.setVerticalBand(0, kMaxChunkYBand);
        worlds.push_back(std::move(w));
    }

    // Fold a chunk coord onto a world's torus (the canonical storage key).
    auto wrapCC = [&](const WorldInstance& w, ChunkCoord c) -> ChunkCoord {
        c.x = w.torus.wrapChunk(c.x);
        c.z = w.torus.wrapChunk(c.z);
        return c;
    };
    // Activate a world's generation profile before touching its chunks.
    auto activate = [&](WorldInstance& w) { voxelworld_set_profile(&w.profile); };

    // Generate + mesh one chunk of a world (canonical key), applying the feature
    // overlays (cave → ore → decoration) the plugin registered. Assumes the world's
    // profile is already active.
    auto loadChunkInto = [&](WorldInstance& w, const ChunkCoord& c) -> bool {
        if (w.meshes.count(c)) return false;
        Chunk* chunk = w.world.loadChunk(c, generator, &w.profile.seed);
        if (!chunk) return false;
        for (const auto& f : pm.featureGenerators())
            if (f.fn)
                f.fn(chunk->origin(), voxelSize, chunkSize, chunk->data(),
                     nullptr, 0, w.profile.seed, f.user_data);
        w.meshes.emplace(c, ChunkMesh::build(*chunk, voxelSize));
        return true;
    };

    // Evict every resident chunk of a world (called for the worlds you are not on,
    // so memory stays bounded to ~one world's resident set).
    auto evictAll = [&](WorldInstance& w) {
        if (w.meshes.empty()) return;
        for (auto& kv : w.meshes) { kv.second.destroy(); w.world.unloadChunk(kv.first); }
        w.meshes.clear();
    };

    // Teleport-and-land on a world's centre: load its origin chunks synchronously so
    // the vertical surface probe sees real terrain, then return a scene spawn point
    // a few metres above the highest solid voxel at the local origin.
    auto settleAtWorld = [&](WorldInstance& w) -> WorldCoord {
        activate(w);
        const ChunkCoord c0 = chunkmath::worldToChunk(WorldCoord(0.5, 80.0, 0.5), voxelSize, chunkSize);
        for (const ChunkCoord& c : w.lod.desiredChunks(c0, "terrain")) loadChunkInto(w, wrapCC(w, c));
        int surfY = 40;
        for (int y = 95; y >= 0; --y)
            if (!w.world.getVoxel(WorldCoord(0.5, y + 0.5, 0.5)).isEmpty()) { surfY = y; break; }
        const double localY = std::max(surfY + 6, 24) + 0.0;
        return WorldCoord(w.offset.x + 0.5, localY, w.offset.y + 0.5);
    };

    // ── Space-view geometry: a starfield + one proxy "planet" per world ─────────
    // Both reuse the ordinary chunk-mesh path; the star shell draws camera-centred
    // (at infinity), the proxies at each world's true scene position (fly-to-able).
    auto proxyPalette = [](int mode) -> uint8_t {
        switch (mode) {
            case VW_DESERT: return 6;   // sand  — tan
            case VW_SNOWY:  return 7;   // snow  — white
            case VW_MOON:   return 1;   // stone — grey
            default:        return 2;   // grass — green (paradise)
        }
    };
    for (auto& up : worlds)
        up->proxyMesh = buildBallMesh(proxyPalette(up->profile.biome_mode), kProxyGrid);
    ChunkMesh    starMesh   = buildStarMesh(kStarGrid, kStarProb);
    const double starVoxelM = kStarRadiusM / (kStarGrid * 0.5 - 3.0);   // shell → kStarRadiusM

    // ── Camera: spawn on paradise ─────────────────────────────────────────────
    float pitch = -0.35f, yaw = 0.0f;
    WorldCoord camPos = settleAtWorld(*worlds[0]);

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    bool cursorCaptured = true, firstMouse = true, prevF = false;
    double lastMouseX = 0.0, lastMouseY = 0.0;
    constexpr int kTeleKeys[] = {GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3, GLFW_KEY_4};
    bool prevTele[std::size(kTeleKeys)] = {};

    Log::info(kLogCat, "Fly UP to leave a world for space, cross the void, and descend on "
                       "another. WASD move, mouse look, Space/Shift up/down, Left-Ctrl fast, "
                       "1-4 teleport, F cursor, ESC quit.");
    auto prevTime = std::chrono::high_resolution_clock::now();

    while (!window.shouldClose()) {
        window.pollEvents();
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        if (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        const bool curF = (glfwGetKey(glfwWin, GLFW_KEY_F) == GLFW_PRESS);
        if (curF && !prevF) {
            cursorCaptured = !cursorCaptured;
            glfwSetInputMode(glfwWin, GLFW_CURSOR,
                             cursorCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            firstMouse = true;
        }
        prevF = curF;

        // Teleport: number keys 1..4 land on each world's centre.
        for (std::size_t i = 0; i < std::size(kTeleKeys); ++i) {
            const bool cur = glfwGetKey(glfwWin, kTeleKeys[i]) == GLFW_PRESS;
            if (cur && !prevTele[i] && i < worlds.size()) {
                camPos = settleAtWorld(*worlds[i]);
                pitch = -0.35f;
            }
            prevTele[i] = cur;
        }

        // Mouse look.
        if (cursorCaptured) {
            double mx, my;
            glfwGetCursorPos(glfwWin, &mx, &my);
            if (!firstMouse) {
                yaw   += static_cast<float>(mx - lastMouseX) * kMouseSens;
                pitch -= static_cast<float>(my - lastMouseY) * kMouseSens;
                pitch = std::clamp(pitch, -1.55f, 1.55f);
            }
            lastMouseX = mx; lastMouseY = my; firstMouse = false;
        }

        // WASD + Space/Shift fly, Left-Ctrl boosts.
        const bool fast = (glfwGetKey(glfwWin, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) ||
                          (glfwGetKey(glfwWin, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS);
        const float speed = fast ? kFastSpeed : kCruiseSpeed;
        const float sp = std::sin(pitch), cp = std::cos(pitch);
        const float sy = std::sin(yaw),   cy = std::cos(yaw);
        const glm::dvec3 fwd  {cp * sy, sp, cp * cy};
        const glm::dvec3 right{cy, 0.0, -sy};
        glm::dvec3 delta{0.0, 0.0, 0.0};
        const double stepM = static_cast<double>(speed * dt);
        if (glfwGetKey(glfwWin, GLFW_KEY_W)          == GLFW_PRESS) delta += fwd   * stepM;
        if (glfwGetKey(glfwWin, GLFW_KEY_S)          == GLFW_PRESS) delta -= fwd   * stepM;
        if (glfwGetKey(glfwWin, GLFW_KEY_A)          == GLFW_PRESS) delta -= right * stepM;
        if (glfwGetKey(glfwWin, GLFW_KEY_D)          == GLFW_PRESS) delta += right * stepM;
        if (glfwGetKey(glfwWin, GLFW_KEY_SPACE)      == GLFW_PRESS) delta.y += stepM;
        if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) delta.y -= stepM;
        glm::dvec3 p = camPos.value + delta;

        // Which world's footprint (grown by a margin) contains the camera XZ?
        WorldInstance* active = nullptr;
        for (auto& up : worlds) {
            WorldInstance* w = up.get();
            const double half = w->torus.halfM() + kFootprintMarginM;
            if (std::abs(p.x - w->offset.x) <= half && std::abs(p.z - w->offset.y) <= half) {
                active = w; break;
            }
        }
        // Below the atmosphere top, engage the torus wrap so flying horizontally
        // loops you around the planet; above it the wrap releases so you can leave.
        if (active && p.y < kAtmoHighM) {
            p.x = active->offset.x + active->torus.wrapCoordM(p.x - active->offset.x);
            p.z = active->offset.y + active->torus.wrapCoordM(p.z - active->offset.y);
        }
        camPos = WorldCoord(p);

        // ── Stream the active world; evict the rest ──────────────────────────
        if (active) {
            activate(*active);
            const double localCamX = camPos.value.x - active->offset.x;
            const double localCamZ = camPos.value.z - active->offset.y;
            const ChunkCoord center = chunkmath::worldToChunk(
                WorldCoord(active->torus.wrapCoordM(localCamX), camPos.value.y,
                           active->torus.wrapCoordM(localCamZ)),
                voxelSize, chunkSize);
            const auto ring = active->lod.desiredChunks(center, "terrain");
            std::unordered_set<ChunkCoord, ChunkCoordHash> desired;
            desired.reserve(ring.size());
            for (const ChunkCoord& c : ring) desired.insert(wrapCC(*active, c));
            int loaded = 0;
            for (const ChunkCoord& c : ring)
                if (loadChunkInto(*active, wrapCC(*active, c)) && ++loaded >= kLoadsPerFrame) break;
            std::vector<ChunkCoord> toEvict;
            for (const auto& kv : active->meshes)
                if (!desired.count(kv.first)) toEvict.push_back(kv.first);
            for (const ChunkCoord& c : toEvict) {
                active->meshes[c].destroy(); active->meshes.erase(c);
                active->world.unloadChunk(c);
            }
        }
        for (auto& up : worlds)
            if (up.get() != active) evictAll(*up);

        // ── Sky / fog / far clip by altitude (the M19 transition) ────────────
        const bool   isMoon = active && active->profile.biome_mode == VW_MOON;
        const double aSky   = active ? clamp01((camPos.value.y - kAtmoLowM) /
                                               (kAtmoHighM - kAtmoLowM)) : 1.0;
        // The "space view": climbed out of the atmosphere, out in the void, or on the
        // airless moon — when the starfield and the sister worlds should be visible.
        // It also switches to the large far clip so those distant bodies are in range.
        const bool spaceView = !active || isMoon || aSky > kSpaceViewAltFrac;
        renderer.setFarClip(spaceView ? kSpaceFarClipM : kSurfaceFarClipM);
        if (active) {
            renderer.setSky(psky::blend(active->daySky, psky::spaceSky(), static_cast<float>(aSky)));
            FogParams f = active->fog;
            f.density = active->fog.density * static_cast<float>(1.0 - aSky);
            renderer.setFog(f);
            renderer.setClearColor(glm::mix(active->tint, kSpaceColor, static_cast<float>(aSky)));
        } else {
            renderer.setSky(psky::spaceSky());
            renderer.setFog(FogParams{});            // density 0 → no fog
            renderer.setClearColor(kSpaceColor);
        }

        // ── Render ───────────────────────────────────────────────────────────
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }

        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);

        if (active) {
            const double localCamX = camPos.value.x - active->offset.x;
            const double localCamZ = camPos.value.z - active->offset.y;
            for (const auto& kv : active->meshes) {
                const Chunk* chunk = active->world.getChunk(kv.first);
                if (!chunk) continue;
                const WorldCoord o = chunk->origin();
                // Draw each chunk at the periodic image nearest the camera's local
                // position, translated into scene space by the world's offset.
                const double sceneOx = active->offset.x +
                    active->torus.nearestOriginM(o.value.x, localCamX);
                const double sceneOz = active->offset.y +
                    active->torus.nearestOriginM(o.value.z, localCamZ);
                renderer.renderChunk(kv.second, WorldCoord(sceneOx, o.value.y, sceneOz), voxelSize);
            }
        }

        // Space view: the starfield + the sister worlds as distant proxy planets.
        if (spaceView) {
            const double pHalf = 0.5 * kProxyGrid * kProxyVoxelM;
            for (auto& up : worlds) {
                WorldInstance* wi = up.get();
                if (wi == active) continue;   // the active world streams for real
                const WorldCoord po(wi->offset.x - pHalf, kProxyCenterYM - pHalf,
                                    wi->offset.y - pHalf);
                renderer.renderChunk(wi->proxyMesh, po, kProxyVoxelM);
            }
            // Star shell centred on the camera → sits at infinity (can't be flown to).
            const double sHalf = 0.5 * kStarGrid * starVoxelM;
            const WorldCoord so(camPos.value.x - sHalf, camPos.value.y - sHalf,
                                camPos.value.z - sHalf);
            renderer.renderChunk(starMesh, so, starVoxelM);
        }

        // HUD.
        const bool inSpace = !active || camPos.value.y >= kAtmoHighM;
        std::string worldName = active ? active->name : "open space";
        std::string biomeLbl = "—";
        double lx = camPos.value.x, lz = camPos.value.z;
        if (active) {
            lx = active->torus.wrapCoordM(camPos.value.x - active->offset.x);
            lz = active->torus.wrapCoordM(camPos.value.z - active->offset.y);
            biomeLbl = (active->profile.biome_mode == VW_MOON)
                           ? "regolith"
                           : std::string(voxelworld_biome_name(lx + 0.5, lz + 0.5));
        }
        char line[224];
        std::snprintf(line, sizeof(line),
                      "seed %llu | world: %s | %s | local %.0f %.0f %.0f | %s | %s",
                      static_cast<unsigned long long>(baseSeed), worldName.c_str(),
                      biomeLbl.c_str(), lx, camPos.value.y, lz,
                      inSpace ? "SPACE" : "ATMOSPHERE", fast ? "FAST" : "cruise");
        renderer.setHudText({std::string(line),
                             "WASD move  mouse look  Space/Shift up/down  Left-Ctrl fast  "
                             "1-4 teleport  F cursor  ESC quit"});

        renderer.render();
    }

    for (auto& up : worlds) {
        for (auto& kv : up->meshes) kv.second.destroy();
        up->proxyMesh.destroy();
    }
    starMesh.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}
