#pragma once

#include "GuardReactionAdapter.h"

namespace SkyrimRemember {

// Sends one universal observation to the adapter for that NPC's role. A
// profile may remember the Player even when it has no reaction adapter yet.
class ProfileReactionRouter {
public:
  void Configure(DebugBoard *board);

  // Returns the hostility verdict of the adapter that handled it; Calm for a
  // profile without one.
  HostilityVerdict OnObserved(RE::Actor &actor, const NpcObservationContext &context, const PerceptionOutcome &outcome, const IdentityObservation &identity);

  // An observation pass is starting: adapters may show what they grouped.
  void BeginPass();

  // Load, revert, or new game.
  void Reset();

private:
  GuardReactionAdapter guards;
};

} // namespace SkyrimRemember
