#include "core/EngineFingerprint.h"

#ifndef LATTICE_BUILD_COMMIT
#define LATTICE_BUILD_COMMIT "unknown"
#endif

namespace core {

namespace {
// A single, unsplit string literal so it survives ordinary `strings`/hex-dump
// inspection of a shipped binary intact. LATTICE_BUILD_COMMIT is injected by
// CMake from `git rev-parse` at configure time (see CMakeLists.txt).
constexpr char kFingerprint[] =
    "LATTICE-ENGINE-FINGERPRINT-v1"
    "|repo=github.com/EpicSalvation/lattice"
    "|commit=" LATTICE_BUILD_COMMIT
    "|license=MIT|copyright=Troy Dontigney";
}  // namespace

const char* engineFingerprint() { return kFingerprint; }

}  // namespace core
