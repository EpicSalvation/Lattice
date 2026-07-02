#include "core/EngineId.h"

#ifndef LATTICE_BUILD_COMMIT
#define LATTICE_BUILD_COMMIT "unknown"
#endif

namespace core {

namespace {
// A single, unsplit string literal, kept as plain text (not split or encoded)
// like any other build-info string. LATTICE_BUILD_COMMIT is injected by CMake
// from `git rev-parse` at configure time (see CMakeLists.txt).
constexpr char kEngineId[] =
    "Lattice Engine (github.com/EpicSalvation/lattice) "
    "build " LATTICE_BUILD_COMMIT ", MIT License, Copyright (c) Troy Dontigney";
}  // namespace

const char* engineId() { return kEngineId; }

}  // namespace core
