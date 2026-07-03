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
// and omit it for a deterministic default. Pass `--size N` to cap the world to a
// square of half-extent N metres (an invisible border you cannot fly past); omit
// it for an infinite world.
//
// Controls: WASD move, mouse look, Space/Shift fly up/down, hold Left-Ctrl to fly
// FAST, F toggles the mouse cursor, ESC quits.

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
    // ── Arguments: [seed] [--size N] ────────────────────────────────────────────
    uint64_t worldSeed = kDefaultSeed;
    bool     bounded   = false;
    double   halfSizeM = 0.0;   // half-extent in metres when bounded
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--size" && i + 1 < argc) {
            halfSizeM = std::strtod(argv[++i], nullptr);
            bounded = halfSizeM > 0.0;
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
                        (bounded ? "  (bounded to +/-" + std::to_string((long long)halfSizeM) + " m)"
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

    const int    chunkSize = terrainLayer->chunkSizeVoxels();
    const double voxelSize = terrainLayer->voxelSizeM();
    // Bounds in chunk coordinates (a chunk is kept only if fully-or-partly inside).
    const int halfChunks = bounded
        ? static_cast<int>(std::floor(halfSizeM / (voxelSize * chunkSize)))
        : 0;
    auto inBounds = [&](const ChunkCoord& c) {
        return !bounded || (std::abs(c.x) <= halfChunks && std::abs(c.z) <= halfChunks);
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

    // ── Camera: start above the surface at the origin ───────────────────────────
    float pitch = -0.35f, yaw = 0.0f;
    WorldCoord camPos(0.5, 80.0, 0.5);
    {
        ChunkCoord c0 = chunkmath::worldToChunk(camPos, voxelSize, chunkSize);
        for (const ChunkCoord& c : lod.desiredChunks(c0, "terrain")) loadChunk(c);
        int surfY = 40;
        for (int y = 95; y >= 0; --y) {
            if (!world.getVoxel(WorldCoord(0.5, y + 0.5, 0.5)).isEmpty()) { surfY = y; break; }
        }
        camPos = WorldCoord(0.5, std::max(surfY + 6, 24) + 0.0, 0.5);
    }

    GLFWwindow* glfwWin = window.glfwHandle();
    glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    bool cursorCaptured = true, firstMouse = true, prevF = false;
    double lastMouseX = 0.0, lastMouseY = 0.0;

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
        // Invisible world border: clamp horizontally so you cannot fly past the edge.
        if (bounded) {
            p.x = std::clamp(p.x, -halfSizeM, halfSizeM);
            p.z = std::clamp(p.z, -halfSizeM, halfSizeM);
        }
        camPos = WorldCoord(p);

        // ── Stream chunks around the camera ─────────────────────────────────────
        ChunkCoord center = chunkmath::worldToChunk(camPos, voxelSize, chunkSize);
        int loaded = 0;
        for (const ChunkCoord& c : lod.desiredChunks(center, "terrain"))
            if (loadChunk(c) && ++loaded >= kLoadsPerFrame) break;
        std::vector<ChunkCoord> toEvict;
        for (const auto& kv : meshes)
            if (lod.shouldEvict(center, kv.first, "terrain") || !inBounds(kv.first))
                toEvict.push_back(kv.first);
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
                      bounded ? "bounded" : "infinite",
                      voxelworld_biome_name(camPos.value.x, camPos.value.z),
                      camPos.value.x, camPos.value.y, camPos.value.z,
                      fast ? "FAST" : "cruise");
        renderer.setHudText({std::string(line),
                             "WASD move  mouse look  Space/Shift up/down  "
                             "Left-Ctrl fast  F cursor  ESC quit"});

        for (const auto& kv : meshes) {
            const Chunk* chunk = terrainLayer->getChunk(kv.first);
            if (chunk) renderer.renderChunk(kv.second, chunk->origin(), voxelSize);
        }
        renderer.render();
    }

    for (auto& kv : meshes) kv.second.destroy();
    renderer.shutdown();
    engine.stop();
    return 0;
}
