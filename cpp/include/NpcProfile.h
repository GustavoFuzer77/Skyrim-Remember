#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

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

// Places one humanoid NPC, or returns no profile for the Player and creatures.
// Cheap, data-driven, and free of side effects: it must not create memory,
// capture appearance, or reach the memory store.
std::optional<NpcProfile> ClassifyNpc(RE::Actor &actor);

// The single switch deciding which profiles receive universal appearance
// observations. Reaction support is routed independently after recognition.
bool IsProfileEnabled(NpcProfile profile);

} // namespace SkyrimRemember
