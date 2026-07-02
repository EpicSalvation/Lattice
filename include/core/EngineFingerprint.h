#pragma once

// Build-provenance marker (docs/architecture.md §19).
//
// Every binary that links this engine embeds the string returned by
// engineFingerprint() in its data segment, independent of whatever attribution
// (or lack thereof) the shipped game displays or whether engine logging is
// enabled at runtime. It exists so the copyright holder can positively
// identify, via a binary inspection tool (`strings`, a hex dump, etc.), that a
// given executable was built against this engine — evidence for enforcing the
// MIT license's attribution requirement, not a technical restriction on use.
// It does not alter engine behavior and cannot be used to disable or gate
// functionality.
namespace core {

const char* engineFingerprint();

}  // namespace core
