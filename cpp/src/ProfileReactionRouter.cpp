#include "ProfileReactionRouter.h"

namespace SkyrimRemember {

void ProfileReactionRouter::Configure(DebugBoard *board) {
  guards.Configure(board);
}

HostilityVerdict ProfileReactionRouter::OnObserved(RE::Actor &actor, const NpcObservationContext &context, const PerceptionOutcome &outcome,
                                                   const IdentityObservation &identity) {
  switch (context.profile) {
  case NpcProfile::Guard:
    return guards.OnObserved(actor, context, outcome, identity);
  case NpcProfile::Follower:
  case NpcProfile::Citizen:
  case NpcProfile::Merchant:
  case NpcProfile::Generic:
    // These profiles already remember appearance. Their reactions arrive as
    // separate adapters without changing the observation or memory layers.
    break;
  }
  return HostilityVerdict::Calm;
}

void ProfileReactionRouter::BeginPass() {
  guards.BeginPass();
}

void ProfileReactionRouter::Reset() {
  guards.Reset();
}

} // namespace SkyrimRemember
