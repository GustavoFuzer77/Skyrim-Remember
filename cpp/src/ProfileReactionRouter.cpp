#include "ProfileReactionRouter.h"

namespace SkyrimRemember {

void ProfileReactionRouter::OnObserved(RE::Actor &actor, const NpcObservationContext &context, const PerceptionOutcome &outcome,
                                       const IdentityObservation &identity) {
  switch (context.profile) {
  case NpcProfile::Guard:
    guards.OnObserved(actor, context, outcome, identity);
    break;
  case NpcProfile::Follower:
  case NpcProfile::Citizen:
  case NpcProfile::Merchant:
  case NpcProfile::Generic:
    // These profiles already remember appearance. Their reactions arrive as
    // separate adapters without changing the observation or memory layers.
    break;
  }
}

void ProfileReactionRouter::BeginPass() {
  guards.BeginPass();
}

void ProfileReactionRouter::Reset() {
  guards.Reset();
}

} // namespace SkyrimRemember
