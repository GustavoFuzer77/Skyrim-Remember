#pragma once

#include "AppearanceMemoryService.h"
#include "NpcObservation.h"

namespace SkyrimRemember {

// Translates universal identity and memory information into guard-specific
// decisions. Skyrim effects live here (or in executors called from here), never
// in AppearanceMemoryService.
class GuardReactionAdapter {
public:
  void OnObserved(RE::Actor &actor, const NpcObservationContext &context, const PerceptionOutcome &outcome, const IdentityObservation &identity);
};

} // namespace SkyrimRemember
