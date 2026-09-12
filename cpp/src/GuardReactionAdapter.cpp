#include "GuardReactionAdapter.h"

namespace SkyrimRemember {

void GuardReactionAdapter::OnObserved(RE::Actor &, const NpcObservationContext &, const PerceptionOutcome &, const IdentityObservation &) {
  // The first reaction policy will be added here. Keeping this boundary inert
  // preserves today's gameplay while appearance memory becomes universal.
}

} // namespace SkyrimRemember
