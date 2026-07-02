#pragma once

// Build identification string (docs/architecture.md §19).
//
// Every binary that links this engine carries the string returned by
// engineId() in its data segment — ordinary build metadata (engine name,
// upstream repo, license, build commit), the same kind of "about" string many
// libraries embed. It's a byproduct of the normal build/version stamping
// engines do, but it's also useful if a copyright holder ever needs to
// confirm a given executable was built against this engine.
namespace core {

const char* engineId();

}  // namespace core
