#include "GuardReactionAdapter.h"
#include "Papyrus.h"

#include <RE/A/Actor.h>
#include <RE/M/Misc.h>
#include <SKSE/SKSE.h>

namespace SkyrimRemember {

void GuardReactionAdapter::Configure(DebugBoard *board) {
  debug = board;
}

HostilityVerdict GuardReactionAdapter::OnObserved(RE::Actor &actor, const NpcObservationContext &context, const PerceptionOutcome &outcome,
                                                  const IdentityObservation &identity) {
  const auto reaction = DecideGuardReaction(identity);
  if (reaction != GuardReaction::None) {
    // Recorded, not shown: the board groups and spaces notices, and the next
    // pass decides what reaches the HUD.
    notices.Record(context.referenceFormId, reaction);
    SKSE::log::info(
        "Guard reaction: observer={:08X} reaction={}", context.referenceFormId, reaction == GuardReaction::Recognized ? "recognized" : "unrecognized");
  }

  const auto settings = debug ? debug->Hostility() : HostilitySettings{};
  const auto suspicion = identity.axes[AxisSlot(Axis::Suspicion)];
  const auto verdict =
      hostility.Decide(context.referenceFormId, identity.appearance.recognized, suspicion, actor.IsInCombat(), GuardHostility::Clock::now(), settings);
  if (verdict == HostilityVerdict::Attack) {
    const auto queued = Papyrus::StartCombatWithPlayer(actor);
    if (queued && debug) {
      debug->CountAttack();
    }
    SKSE::log::info("Guard hostility: observer={:08X} suspicion={:.1f} threshold={:.1f} distance={:.0f} combatQueued={}",
                    context.referenceFormId,
                    suspicion,
                    settings.suspicionThreshold,
                    outcome.distance,
                    queued);
  }
  return verdict;
}

void GuardReactionAdapter::BeginPass() {
  const auto summary = notices.Flush(GuardNoticeBoard::Clock::now());
  if (summary.Empty()) {
    return;
  }
  for (const auto &[reaction, count] :
       {std::pair{GuardReaction::Recognized, summary.recognized}, std::pair{GuardReaction::Unrecognized, summary.unrecognized}}) {
    const auto text = FormatGuardNotice(reaction, count);
    if (!text.empty()) {
      RE::DebugNotification(text.c_str());
    }
  }
  SKSE::log::info("Guard notice: recognized={} unrecognized={} tracked={}", summary.recognized, summary.unrecognized, notices.Tracked());
}

void GuardReactionAdapter::Reset() {
  notices.Reset();
  hostility.Reset();
}

} // namespace SkyrimRemember
