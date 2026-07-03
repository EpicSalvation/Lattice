// Demo — Voxel World: a Minecraft-style infinite survival world.
//
// A sibling to the mega-demo (demos/20), but where that demo is a combat/mining
// survival slice on a single rolling heightmap, THIS demo's job is worldgen
// breadth and seamless streaming: SIX biomes (Ocean, Plains, Forest, Desert,
// Mountains, Snowy Tundra) generated deterministically from a world seed, streamed
// infinitely in all four horizontal directions so the world never ends — or,
// optionally, capped to a finite size to demonstrate a classic "world border".
//
// There is no mining/combat/mobs here (that is the mega-demo's job). You FLY: a
// normal cruise speed for looking around, and a much faster boost for covering the
// distance of a bounded Minecraft world and then flying on to prove there's no edge.
//
// Choosing a world: pass a seed as the first argument — `21-voxel-world 12345` —
// and omit it for a deterministic default. World topology is one of three:
//   (default)  infinite — streams forever in all four horizontal directions.
//   --size N   bounded  — an invisible hard border at half-extent N metres.
//   --wrap N   wrapping — a torus of half-extent N: fly past one edge and you
//              reappear at the opposite one, like the surface of a planet. The
//              seam is made continuous by a transitional swath of terrain blended
//              toward the wrapped-around side (see WorldWrap.h and the voxel-world
//              plugin's fbm2 seam blend), so there is no cliff where you cross. N
//              snaps up to a whole, even number of chunks (min the streaming
//              diameter). Reusable via demos/common/WorldWrap.h (the planned M19
//              "No Man's Voxel" demo shares this topology).
//
// Controls: WASD move, mouse look, Space/Shift fly up/down, hold Left-Ctrl to fly
// FAST, F toggles the mouse cursor, ESC quits. Number keys 1..6 teleport along +X
// to escalating distances (5 = the classic 30,000,000 m Minecraft border, 6 = the
// engine's real edge) so you need not fly the whole way.
//
// On the "infinite" world's limits: a world position is a double (WorldCoord), which
// is precise to ~7 nm even at the Minecraft border, so the double never runs out of
// resolution anywhere a player would go. The real edge is ChunkCoord being int32: a
// chunk index would overflow it around 6.9e10 m out, so horizontal position is
// clamped safely short of that at kEngineLimitM (1000x the Minecraft border).

#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "platform/Window.h"
#include "renderer/BgfxRenderer.h"
#include "renderer/ChunkMesh.h"
#include "renderer/LODManager.h"
#include "renderer/TextureManager.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/World.h"

#include "../common/WorldWrap.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef VOXEL_WATER_PLUGIN_PATH
#  define VOXEL_WATER_PLUGIN_PATH ""
#endif

// The voxel-world worldgen plugin is COMPILED INTO this demo (its plugin.cpp is a
// target source, guarded by VOXELWORLD_COMPILED_IN) so the demo can call its custom
// entry points directly — the biome HUD query and the seed pointer, which a disk-
// loaded MODULE would not expose to the host. The water plugin stays disk-loaded
// (it only needs its feature generator, resolved through the hook table).
extern "C" int         voxelworld_plugin_init(PluginContext* ctx);
extern "C" void        voxelworld_set_seed_ptr(uint64_t* ptr);
extern "C" void        voxelworld_set_wrap(double period_m, double band_m);
extern "C" const char* voxelworld_biome_name(double wx, double wz);

namespace {
constexpr char     kLogCat[]       = "demo21";
constexpr int      kLoadsPerFrame  = 4;
constexpr float    kCruiseSpeed    = 30.0f;   // m/s — normal exploration
constexpr float    kFastSpeed      = 160.0f;  // m/s — hold Left-Ctrl to cover ground
constexpr float    kMouseSens      = 0.002f;
constexpr uint64_t kDefaultSeed    = 0xA11CE5EEDull;  // the mega-demo's seed pattern
// Terrain heights span ~7 (seabed) .. ~80 (peaks); a 3-band box (chunk 32) covers
// the whole vertical range without streaming empty sky or deep void.
constexpr int      kMaxChunkYBand  = 2;
constexpr double   kMcBorderM      = 30000000.0;  // Minecraft's classic world border

// The world streams "infinitely," but a chunk index is int32 (ChunkCoord, and a
// chunk is 32 m here), so the true edge is where that index would overflow int32
// — roughly 6.9e10 m out. We clamp horizontal position well inside that, at 1000x
// the Minecraft border. That is also far past where double precision could ever
// matter: the ULP of a double at this range is only a few micrometres, versus the
// ~7 nanometres it already is at the Minecraft border.
constexpr double   kEngineLimitM   = 3.0e10;

// Teleport presets (metres along +X) reachable with number keys 1..6, so you can
// jump to the Minecraft border — or the engine's real edge — without the long fly.
constexpr double   kTeleportPresetsM[] = {
    0.0, 1000.0, 100000.0, 1000000.0, kMcBorderM, kEngineLimitM};

// ───────────────────────── Texture synthesis (PNG) ──────────────────────────
// A self-contained 16px-tile PNG writer, mirrored from the mega-demo so the demo
// has no external asset dependency — the voxel-world plugin registers these tile
// ids by path and we synthesise them here on first run.
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
}  // namespace

int main(int argc, char** argv) {
    // ── Arguments: [seed] [--size N | --wrap N] ─────────────────────────────────
    // --size caps the world with an invisible hard border; --wrap makes it a torus
    // of half-extent N that loops seamlessly. They are mutually exclusive world
    // topologies — the last one on the command line wins.
    uint64_t worldSeed   = kDefaultSeed;
    bool     bounded     = false;
    bool     wrapping    = false;
    double   halfSizeM   = 0.0;   // half-extent in metres when bounded
    double   wrapHalfReqM = 0.0;  // requested wrap half-extent (snapped to whole chunks below)
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--size" && i + 1 < argc) {
            halfSizeM = std::strtod(argv[++i], nullptr);
            bounded = halfSizeM > 0.0;
            if (bounded) wrapping = false;
        } else if (arg == "--wrap" && i + 1 < argc) {
            wrapHalfReqM = std::strtod(argv[++i], nullptr);
            wrapping = wrapHalfReqM > 0.0;
            if (wrapping) bounded = false;
        } else if (arg == "--seed" && i + 1 < argc) {
            worldSeed = std::strtoull(argv[++i], nullptr, 0);
        } else if (!arg.empty() && arg[0] != '-') {
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(arg.c_str(), &end, 0);
            if (end && *end == '\0') worldSeed = parsed;
            else Log::warn(kLogCat, "Could not parse seed argument; using the default.");
        }
    }
    Log::info(kLogCat, (std::string("World seed: ") + std::to_string(worldSeed) +
                        (bounded  ? "  (bounded to +/-" + std::to_string((long long)halfSizeM) + " m)"
                       : wrapping ? "  (wrapping)"
                                  : "  (infinite)")).c_str());

#ifdef VOXEL_REPO_ROOT
    if (!std::filesystem::exists("assets")) {
        std::error_code ec; std::filesystem::current_path(VOXEL_REPO_ROOT, ec);
    }
#endif
    ensureTextures("assets/textures/voxelworld");

    // ── Layer config: one terminal "terrain" layer streamed around the camera ───
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

    platform::Window window(1280, 720, "VoxelEngine — Demo: Voxel World");
    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH));
    renderer.setCrosshair(false);
    renderer.setFarClip(500.0f);
    FogParams fog; fog.color = glm::vec3(0.62f, 0.74f, 0.86f);
    fog.near_m = 200.0f; fog.far_m = 460.0f; fog.density = 1.0f;
    renderer.setFog(fog);
    renderer.setClearColor(glm::vec3(0.62f, 0.74f, 0.86f));

    texture::TextureManager textureManager(pm, renderer);

    // ── Plugins: worldgen (biomes) + water (flat sea) ───────────────────────────
    auto loadOrDie = [&](const char* path, const char* name) {
        if (std::string(path).empty() || pm.loadPlugin(path) == kInvalidPluginId) {
            Log::error(kLogCat, (std::string("Fatal: could not load ") + name +
                                 " plugin from '" + path + "'.").c_str());
            std::exit(1);
        }
    };
    pm.wireInPlugin(voxelworld_plugin_init);   // compiled-in worldgen (see extern decl above)
    loadOrDie(VOXEL_WATER_PLUGIN_PATH, "water");
    voxelworld_set_seed_ptr(&worldSeed);

    textureManager.rebuild();
    renderer.setAtlas(textureManager.atlas());
    Log::info(kLogCat, (std::string("Texture atlas: ") +
                        std::to_string(textureManager.tileCount()) + " tiles.").c_str());

    // Terrain generator + feature overlays, applied in registration order:
    // voxel-world's cave -> ore -> decoration, then water's water_table (loaded
    // last) floods empty space at/below sea level. Decoration gates on a grassy
    // cap (sand for cacti), and the terrain generator makes any shoreline sandy,
    // so land decoration never roots below the sea it is later flooded with.
    LayerGeneratorFn generator = nullptr;
    for (const auto& g : pm.layerGenerators())
        if (g.layer_name == "terrain") generator = g.fn;
    if (!generator) { Log::error(kLogCat, "Fatal: no 'terrain' generator."); return 1; }

    World world(layerConfig);
    Layer* terrainLayer = world.layer("terrain");
    if (!terrainLayer) { Log::error(kLogCat, "Fatal: expected a terrain layer."); return 1; }

    LODManager lod(layerConfig);
    lod.setVerticalBand(0, kMaxChunkYBand);

    const int    chunkSize       = terrainLayer->chunkSizeVoxels();
    const double voxelSize       = terrainLayer->voxelSizeM();
    const double chunkWorldSizeM = voxelSize * chunkSize;

    // ── World topology ──────────────────────────────────────────────────────────
    // --size: an invisible hard border, kept in chunk coordinates (a chunk is kept
    // if fully-or-partly inside).
    const int halfChunks = bounded
        ? static_cast<int>(std::floor(halfSizeM / chunkWorldSizeM))
        : 0;
    auto inBounds = [&](const ChunkCoord& c) {
        return !bounded || (std::abs(c.x) <= halfChunks && std::abs(c.z) <= halfChunks);
    };

    // --wrap: a torus whose period is a whole EVEN number of chunks, so the two
    // sides of a seam reuse identical chunk data and the domain is origin-centred.
    // The period is snapped up to at least the streaming diameter so no chunk ever
    // needs to appear on both sides of the camera at once. The metric period plus a
    // transitional blend band go to the worldgen (voxelworld_set_wrap) so terrain is
    // continuous across the seam.
    worldwrap::Torus torus;
    if (wrapping) {
        const int viewDist     = layerConfig.findLayer("terrain")->view_distance_chunks;
        const int minPeriod    = 2 * viewDist + 2;
        int periodChunks = static_cast<int>(std::llround(2.0 * wrapHalfReqM / chunkWorldSizeM));
        periodChunks = std::max(periodChunks, minPeriod);
        periodChunks += (periodChunks & 1);                 // force even
        torus = worldwrap::Torus{periodChunks, chunkWorldSizeM};
        const double bandM = std::max(chunkWorldSizeM, torus.periodM() * 0.06);
        voxelworld_set_wrap(torus.periodM(), bandM);
        Log::info(kLogCat, (std::string("Wrapping world: period ") +
                            std::to_string((long long)torus.periodM()) + " m (" +
                            std::to_string(periodChunks) + " chunks), seam band " +
                            std::to_string((long long)bandM) + " m.").c_str());
    }

    // Fold a chunk coord onto the torus (identity unless wrapping) — the canonical
    // key under which the chunk's data is stored and generated.
    auto wrapCC = [&](ChunkCoord c) -> ChunkCoord {
        if (torus.enabled()) { c.x = torus.wrapChunk(c.x); c.z = torus.wrapChunk(c.z); }
        return c;
    };

    // Feature overlays applied after the base generator fills a chunk.
    auto applyFeatures = [&](Chunk& chunk) {
        for (const auto& f : pm.featureGenerators())
            if (f.fn)
                f.fn(chunk.origin(), voxelSize, chunkSize, chunk.data(),
                     nullptr, 0, worldSeed, f.user_data);
    };

    std::unordered_map<ChunkCoord, ChunkMesh, ChunkCoordHash> meshes;
    auto loadChunk = [&](const ChunkCoord& c) -> bool {
        if (meshes.count(c) || !inBounds(c)) return false;
        Chunk* chunk = terrainLayer->loadChunk(c, generator, &worldSeed);
        if (!chunk) return false;
        applyFeatures(*chunk);
        meshes.emplace(c, ChunkMesh::build(*chunk, voxelSize));
        return true;
    };

    // Place the camera above the surface at (x, z): loads the destination chunks
    // synchronously so the vertical surface probe sees real terrain, then returns a
    // spawn point a few metres above the highest solid voxel. Used for the initial
    // spawn and for every teleport.
    auto settleAt = [&](double x, double z) -> WorldCoord {
        WorldCoord   probe(x, 80.0, z);
        ChunkCoord   c0 = chunkmath::worldToChunk(probe, voxelSize, chunkSize);
        for (const ChunkCoord& c : lod.desiredChunks(c0, "terrain")) loadChunk(wrapCC(c));
        int surfY = 40;
        for (int y = 95; y >= 0; --y) {
            if (!world.getVoxel(WorldCoord(x, y + 0.5, z)).isEmpty()) { surfY = y; break; }
        }
        return WorldCoord(x, std::max(surfY + 6, 24) + 0.0, z);
    };

    // ── Camera: start above the surface at the origin ───────────────────────────
    float pitch = -0.35f, yaw = 0.0f;
    WorldCoord camPos = settleAt(0.5, 0.5);

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    bool cursorCaptured = true, firstMouse = true, prevF = false;
    double lastMouseX = 0.0, lastMouseY = 0.0;
    constexpr int kTeleKeys[] = {GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3,
                                 GLFW_KEY_4, GLFW_KEY_5, GLFW_KEY_6};
    bool prevTele[std::size(kTeleKeys)] = {};

    Log::info(kLogCat, "Fly the world. WASD move, mouse look, Space/Shift up/down, "
                       "hold Left-Ctrl to fly fast, F cursor, ESC quit.");
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

        // Teleport: number keys 1..6 jump along +X to escalating distances (up to
        // the Minecraft border and the engine's real edge), each clamped to the
        // world bounds so a preset never lands past a border or the int32 limit.
        for (std::size_t i = 0; i < std::size(kTeleKeys); ++i) {
            const bool cur = glfwGetKey(glfwWin, kTeleKeys[i]) == GLFW_PRESS;
            if (cur && !prevTele[i]) {
                double tx = kTeleportPresetsM[i];
                if (bounded)              tx = std::clamp(tx, -halfSizeM, halfSizeM);
                else if (torus.enabled()) tx = torus.wrapCoordM(tx);
                tx = std::clamp(tx, -kEngineLimitM, kEngineLimitM);
                camPos = settleAt(tx + 0.5, 0.5);
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

        // WASD + Space/Shift fly, Left-Ctrl boosts to fast traversal speed.
        const bool fast = (glfwGetKey(glfwWin, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) ||
                          (glfwGetKey(glfwWin, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS);
        const float speed = fast ? kFastSpeed : kCruiseSpeed;
        const float sp = std::sin(pitch), cp = std::cos(pitch);
        const float sy = std::sin(yaw),   cy = std::cos(yaw);
        const glm::dvec3 fwd  {cp * sy, sp, cp * cy};
        const glm::dvec3 right{cy, 0.0, -sy};
        glm::dvec3 delta{0.0, 0.0, 0.0};
        const double step = static_cast<double>(speed * dt);
        if (glfwGetKey(glfwWin, GLFW_KEY_W)          == GLFW_PRESS) delta += fwd   * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_S)          == GLFW_PRESS) delta -= fwd   * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_A)          == GLFW_PRESS) delta -= right * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_D)          == GLFW_PRESS) delta += right * step;
        if (glfwGetKey(glfwWin, GLFW_KEY_SPACE)      == GLFW_PRESS) delta.y += step;
        if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) delta.y -= step;
        glm::dvec3 p = camPos.value + delta;
        if (bounded) {
            // --size: invisible hard border you cannot fly past.
            p.x = std::clamp(p.x, -halfSizeM, halfSizeM);
            p.z = std::clamp(p.z, -halfSizeM, halfSizeM);
        } else if (torus.enabled()) {
            // --wrap: cross an edge and reappear at the opposite one (a torus).
            p.x = torus.wrapCoordM(p.x);
            p.z = torus.wrapCoordM(p.z);
        }
        // Engine edge: even an "infinite" world is capped short of the int32 chunk
        // overflow (see kEngineLimitM), so free-flight can never wrap chunk indices.
        p.x = std::clamp(p.x, -kEngineLimitM, kEngineLimitM);
        p.z = std::clamp(p.z, -kEngineLimitM, kEngineLimitM);
        camPos = WorldCoord(p);

        // ── Stream chunks around the camera ─────────────────────────────────────
        ChunkCoord center = chunkmath::worldToChunk(camPos, voxelSize, chunkSize);
        int loaded = 0;
        std::vector<ChunkCoord> toEvict;
        if (torus.enabled()) {
            // The camera stays in the canonical domain, but its streaming ring can
            // spill over a seam; fold each desired chunk onto the torus. Because
            // LOD's raw distance metric is meaningless on folded keys, drive eviction
            // off the desired set directly (load nearest-first, drop the rest).
            const auto ring = lod.desiredChunks(center, "terrain");
            std::unordered_set<ChunkCoord, ChunkCoordHash> desired;
            desired.reserve(ring.size());
            for (const ChunkCoord& c : ring) desired.insert(wrapCC(c));
            for (const ChunkCoord& c : ring)
                if (loadChunk(wrapCC(c)) && ++loaded >= kLoadsPerFrame) break;
            for (const auto& kv : meshes)
                if (!desired.count(kv.first)) toEvict.push_back(kv.first);
        } else {
            for (const ChunkCoord& c : lod.desiredChunks(center, "terrain"))
                if (loadChunk(c) && ++loaded >= kLoadsPerFrame) break;
            for (const auto& kv : meshes)
                if (lod.shouldEvict(center, kv.first, "terrain") || !inBounds(kv.first))
                    toEvict.push_back(kv.first);
        }
        for (const ChunkCoord& c : toEvict) {
            meshes[c].destroy(); meshes.erase(c);
            terrainLayer->unloadChunk(c);
        }

        // ── Render ──────────────────────────────────────────────────────────────
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) { fbW = w; fbH = h; renderer.setViewport(w, h); }

        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);

        char line[192];
        std::snprintf(line, sizeof(line),
                      "seed %llu | %s | biome %s | xyz %.0f %.0f %.0f | %s",
                      static_cast<unsigned long long>(worldSeed),
                      bounded ? "bounded" : (wrapping ? "wrapping" : "infinite"),
                      voxelworld_biome_name(camPos.value.x, camPos.value.z),
                      camPos.value.x, camPos.value.y, camPos.value.z,
                      fast ? "FAST" : "cruise");
        renderer.setHudText({std::string(line),
                             "WASD move  mouse look  Space/Shift up/down  Left-Ctrl fast  "
                             "1-6 teleport (5=MC border)  F cursor  ESC quit"});

        for (const auto& kv : meshes) {
            const Chunk* chunk = terrainLayer->getChunk(kv.first);
            if (!chunk) continue;
            WorldCoord origin = chunk->origin();
            // On a torus a chunk's canonical origin may be a whole world away; draw
            // it at the periodic image nearest the camera so it tiles seamlessly.
            if (torus.enabled())
                origin = WorldCoord(torus.nearestOriginM(origin.value.x, camPos.value.x),
                                    origin.value.y,
                                    torus.nearestOriginM(origin.value.z, camPos.value.z));
            renderer.renderChunk(kv.second, origin, voxelSize);
        }
        renderer.render();
    }

    for (auto& kv : meshes) kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}
