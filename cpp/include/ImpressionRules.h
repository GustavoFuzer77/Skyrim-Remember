#pragma once

#include "RustBridge.h"

// Finds the impression rule files, hands them to the Rust core, and resolves
// every keyword and Form they mention against the loaded game data.
//
// What an item makes an NPC feel is data, not code: adding a rule never means
// touching C++. See the "Impression rules" section of the README for the file
// format.
namespace SkyrimRemember::ImpressionRules {

// Where rule files live, relative to the game folder, and the suffix that marks
// one. Every mod ships its own file, so mods never overwrite each other.
inline constexpr const char *directory = "Data/SKSE/Plugins/SkyrimRemember";
inline constexpr const char *fileSuffix = "_Rules.json";

// Loads every rule file in name order, then binds what they reference. Call
// once, on the main thread, after kDataLoaded. A broken file is logged and
// skipped; the others still load.
RulesSummary Load(const MemoryEngine *engine);

} // namespace SkyrimRemember::ImpressionRules
