#include "NpcObservation.h"
#include "ObservationTuning.h"

#include <RE/A/Actor.h>
#include <RE/B/BGSLocation.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESWorldSpace.h>

#include <cmath>

namespace SkyrimRemember {
namespace {
std::uint32_t FormIdOf(const RE::TESForm *form) {
  return form ? form->GetFormID() : 0U;
}
} // namespace

const char *VerdictName(PerceptionVerdict verdict) {
  switch (verdict) {
  case PerceptionVerdict::Observed:
    return "observed";
  case PerceptionVerdict::NotAlive:
    return "notAlive";
  case PerceptionVerdict::NotLoaded:
    return "notLoaded";
  case PerceptionVerdict::OutOfRange:
    return "outOfRange";
  case PerceptionVerdict::Undetected:
    return "undetected";
  case PerceptionVerdict::NoLineOfSight:
    return "noLineOfSight";
  }
  return "unknown";
}

PerceptionOutcome PerceivePlayer(RE::Actor &actor, RE::PlayerCharacter &player) {
  PerceptionOutcome outcome;

  if (&actor == &player || actor.IsDead()) {
    outcome.verdict = PerceptionVerdict::NotAlive;
    return outcome;
  }
  if (!actor.Is3DLoaded()) {
    outcome.verdict = PerceptionVerdict::NotLoaded;
    return outcome;
  }

  // Same worldspace, or — indoors, where there is none — the same cell.
  const auto *actorWorld = actor.GetWorldspace();
  const auto *playerWorld = player.GetWorldspace();
  if (actorWorld != playerWorld) {
    outcome.verdict = PerceptionVerdict::OutOfRange;
    return outcome;
  }
  if (!actorWorld && actor.GetParentCell() != player.GetParentCell()) {
    outcome.verdict = PerceptionVerdict::OutOfRange;
    return outcome;
  }
  outcome.distance = actor.GetPosition().GetDistance(player.GetPosition());
  if (!std::isfinite(outcome.distance) ||
      outcome.distance > ObservationTuning::maximumObservationDistance) {
    outcome.verdict = PerceptionVerdict::OutOfRange;
    return outcome;
  }

  // The two expensive calls, last and in this order: detection is cheaper than
  // the ray cast behind line of sight.
  outcome.detectionLevel = actor.RequestDetectionLevel(&player);
  if (outcome.detectionLevel <= 0) {
    outcome.verdict = PerceptionVerdict::Undetected;
    return outcome;
  }

  bool lineOfSightArgument = false;
  if (!actor.HasLineOfSight(&player, lineOfSightArgument)) {
    outcome.verdict = PerceptionVerdict::NoLineOfSight;
    return outcome;
  }

  outcome.verdict = PerceptionVerdict::Observed;
  return outcome;
}

NpcObservationContext DescribeObservation(RE::Actor &actor, NpcProfile profile) {
  NpcObservationContext context;
  context.profile = profile;
  context.referenceFormId = actor.GetFormID();
  context.baseFormId = FormIdOf(actor.GetBaseObject());
  context.cellFormId = FormIdOf(actor.GetParentCell());
  context.worldspaceFormId = FormIdOf(actor.GetWorldspace());
  context.locationFormId = FormIdOf(actor.GetCurrentLocation());
  context.persistentReference = actor.IsPersistent();
  return context;
}

} // namespace SkyrimRemember
