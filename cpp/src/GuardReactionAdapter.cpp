#include "GuardReactionAdapter.h"

#include <RE/M/Misc.h>
#include <SKSE/SKSE.h>

namespace SkyrimRemember {

void GuardReactionAdapter::OnObserved(RE::Actor &, const NpcObservationContext &context, const PerceptionOutcome &, const IdentityObservation &identity) {
  const auto reaction = DecideGuardReaction(identity);
  if (reaction == GuardReaction::None) {
    return;
  }
  // Recorded, not shown: the board groups and spaces notices, and the next
  // pass decides what reaches the HUD.
  notices.Record(context.referenceFormId, reaction);
  SKSE::log::info(
      "Guard reaction: observer={:08X} reaction={}", context.referenceFormId, reaction == GuardReaction::Recognized ? "recognized" : "unrecognized");
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
}

} // namespace SkyrimRemember
