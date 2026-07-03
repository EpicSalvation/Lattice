#pragma once

#include <cmath>

// ── Toroidal world wrap (demo-level) ─────────────────────────────────────────
//
// A wrapping world is a torus: fly past one horizontal edge and you continue from
// the opposite edge, so a finite world feels edgeless — the local-planet topology
// the voxel-world demo and the planned M19 "No Man's Voxel" demo both want.
//
// Like the demo's --size hard border, wrap topology lives in the DEMO, not the
// engine core (the engine's chunk math stays a plain unbounded int32 grid). This
// header owns only the pure coordinate math the streaming/render loop needs; the
// worldgen SEAM is made continuous separately, by the voxel-world plugin blending
// a transitional band toward the wrapped-around terrain (voxelworld_set_wrap).
//
// The torus period is a WHOLE number of chunks per horizontal axis so the two
// sides of a seam reuse byte-for-byte identical chunk data — the wrap identifies
// them as the same chunks. It is centred on the origin: canonical positions live
// in [-half, +half) and canonical chunk indices in [-period/2, +period/2), so the
// spawn at the origin sits in pristine interior terrain, far from the seam band.
namespace worldwrap {

struct Torus {
    int    periodChunks    = 0;    ///< whole chunks per horizontal axis; 0 = not wrapping (even)
    double chunkWorldSizeM = 0.0;  ///< world size of one chunk along an axis, in metres

    bool   enabled() const { return periodChunks > 0; }
    double periodM() const { return periodChunks * chunkWorldSizeM; }
    double halfM()   const { return 0.5 * periodM(); }

    /// Wrap a world coordinate into the canonical domain [-half, +half).
    double wrapCoordM(double c) const {
        const double P = periodM();
        return c - P * std::floor((c + halfM()) / P);
    }

    /// Wrap a chunk index into the canonical range [-period/2, +period/2).
    /// periodChunks is required even, so the range is symmetric about the origin.
    int wrapChunk(int c) const {
        int m = c % periodChunks;
        if (m < 0) m += periodChunks;          // fold into [0, period)
        if (m >= periodChunks / 2) m -= periodChunks;
        return m;
    }

    /// The image of a canonical chunk-origin coordinate nearest the camera: the
    /// wrapped world repeats every period, so a far-side chunk renders adjacent to
    /// the camera by shifting its origin by whole periods until it is closest.
    double nearestOriginM(double canonOriginM, double camM) const {
        const double P = periodM();
        return canonOriginM - P * std::round((canonOriginM - camM) / P);
    }
};

}  // namespace worldwrap
