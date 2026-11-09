#pragma once

#include "AppearanceMemoryService.h"
#include "GuardNotices.h"
#include "NpcObservation.h"

namespace SkyrimRemember {

// What one guard's observation means, before any spam control.
//
// A change into recognition is a reaction, including the first one of a
// session or of a new encounter. The very first sighting is not: the memory store
// calls a visible face "recognized" even when the guard has no memory yet, and
// a stranger learning the Player's face does not recognize anyone.
constexpr GuardReaction DecideGuardReaction(const IdentityObservation &identity) {
  if (!identity.recognitionChanged) {
    return GuardReaction::None;
  }
  if (!identity.appearance.recognized) {
    return GuardReaction::Unrecognized;
  }
  return identity.newObserver ? GuardReaction::None : GuardReaction::Recognized;
}

// Translates universal identity and memory information into guard-specific
// decisions. Skyrim effects live here (or in executors called from here), never
// in AppearanceMemoryService.
class GuardReactionAdapter {
public:
  void OnObserved(RE::Actor &actor, const NpcObservationContext &context, const PerceptionOutcome &outcome, const IdentityObservation &identity);

  // Shows whatever the notice board has grouped since the last pass.
  void BeginPass();

  void Reset();

private:
  GuardNoticeBoard notices;
};

} // namespace SkyrimRemember
