#pragma once

namespace RE {
class Actor;
} // namespace RE

namespace SkyrimRemember {

class MemoryStore;

namespace Papyrus {

// The script class scripts call into: Data/Source/Scripts/SkyrimRemember.psc.
inline constexpr const char *scriptClass = "SkyrimRemember";

// Registers the native functions of SkyrimRemember.psc. The store must outlive
// every script call, which the runtime's function-local static guarantees.
bool Register(MemoryStore *store);

// Asks the game to start combat between an NPC and the Player, through the
// same Actor.StartCombat a script would call. Runs asynchronously on the
// script VM. Returns false when the request could not even be queued.
bool StartCombatWithPlayer(RE::Actor &actor);

} // namespace Papyrus
} // namespace SkyrimRemember
