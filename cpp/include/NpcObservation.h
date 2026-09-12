#pragma once

#include "NpcProfile.h"

#include <cstdint>

namespace RE {
class Actor;
class PlayerCharacter;
} // namespace RE

namespace SkyrimRemember {

// Why an NPC did or did not perceive the Player on one pass. Ordered from the
// cheapest test to the most expensive, which is also the order they run in.
enum class PerceptionVerdict : std::uint32_t { Observed = 0, NotAlive, NotLoaded, OutOfRange, Undetected, NoLineOfSight };

const char *VerdictName(PerceptionVerdict verdict);

struct PerceptionOutcome {
  PerceptionVerdict verdict = PerceptionVerdict::NotAlive;
  float distance = 0.0F;
  std::int32_t detectionLevel = 0;

  bool Perceived() const {
    return verdict == PerceptionVerdict::Observed;
  }
};

// The one and only implementation of "can this NPC perceive the Player right
// now". Everything that needs the answer — the periodic scan and the equipment
// transition alike — must come through here, so the rules cannot drift apart.
//
// Deliberately excludes the profile: what an actor *is* belongs to
// NpcProfile.h, what it can *see* belongs here.
PerceptionOutcome PerceivePlayer(RE::Actor &actor, RE::PlayerCharacter &player);

// Everything known about one observation, gathered while the Actor is valid.
//
// referenceFormId is the identity. The location fields are metadata about
// *where* the observation happened and must never become part of the key: a
// persistent guard who walks out of Whiterun is the same guard, and folding
// the location into the identity would split his memory in two.
//
// The unsolved problem is the opposite one — Skyrim resetting or recycling a
// generic reference, after which the same FormID stands for a conceptually new
// person. persistentReference and baseFormId are recorded so that a respawn
// policy can be decided later; nothing here claims an identity guarantee the
// game does not offer.
struct NpcObservationContext {
  std::uint32_t referenceFormId = 0;
  std::uint32_t baseFormId = 0;
  std::uint32_t cellFormId = 0;
  std::uint32_t worldspaceFormId = 0;
  std::uint32_t locationFormId = 0;
  NpcProfile profile = NpcProfile::Generic;
  bool persistentReference = false;
};

// Reads the context out of a live Actor. The Actor is not retained: everything
// the caller may keep afterwards is a FormID.
NpcObservationContext DescribeObservation(RE::Actor &actor, NpcProfile profile);

} // namespace SkyrimRemember
