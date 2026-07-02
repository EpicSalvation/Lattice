#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// @brief Per-layer chunk residency/decomposition counts, part of an EngineMetrics snapshot.
struct LayerChunkMetrics {
    std::string layerName;
    size_t      residentChunks   = 0;
    size_t      decomposedMacros = 0;
};

/// @brief Snapshot of engine-wide performance metrics, returned by Engine::getMetrics.
struct EngineMetrics {
    double  frameTimeSec  = 0.0;
    size_t  drawCalls     = 0;
    size_t  voiceCount    = 0;
    size_t  decompInFlight = 0;
    std::vector<LayerChunkMetrics> layers;

    /// @return the sum of residentChunks across every layer.
    size_t totalResidentChunks() const {
        size_t n = 0;
        for (const auto& l : layers) n += l.residentChunks;
        return n;
    }
};
