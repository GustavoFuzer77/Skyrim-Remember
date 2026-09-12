#pragma once

#include <cstddef>
#include <cstdint>

namespace RE {
class Actor;
}

namespace SkyrimRemember {

// What kind of person an NPC is, as far as this mod is concerned.
//
// There is deliberately no Hostile profile: hostility is a state, not a role,
// and an actor the classifier cannot place — including a hostile one — falls
// into Generic rather than getting a category of its own.
enum class NpcProfile : std::uint32_t { Guard = 0, Follower = 1, Citizen = 2, Merchant = 3, Generic = 4 };

inline constexpr std::size_t npcProfileCount = 5;

const char *ProfileName(NpcProfile profile);

// Resolves the Forms classification depends on. Call once after kDataLoaded.
// Returns false when the game data the classifier needs is missing.
bool InitializeClassification();

// Places one actor. Cheap, data-driven, and free of side effects: it must not
// create memory, capture appearance, or reach the Rust core.
NpcProfile ClassifyNpc(RE::Actor &actor);

// The single switch deciding which profiles take part in observation.
//
// Enabling a profile here is all it should take to bring it into the scan; the
// actor discovery must never test for a specific profile itself.
bool IsProfileEnabled(NpcProfile profile);

} // namespace SkyrimRemember
