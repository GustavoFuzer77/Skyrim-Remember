#pragma once

#include "GuardReactionAdapter.h"

namespace SkyrimRemember {

// Sends one universal observation to the adapter for that NPC's role. A
// profile may remember the Player even when it has no reaction adapter yet.
class ProfileReactionRouter {
public:
  void OnObserved(RE::Actor &actor, const NpcObservationContext &context, const PerceptionOutcome &outcome, const IdentityObservation &identity);

  // An observation pass is starting: adapters may show what they grouped.
  void BeginPass();

  // Load, revert, or new game.
  void Reset();

private:
  GuardReactionAdapter guards;
};

} // namespace SkyrimRemember
