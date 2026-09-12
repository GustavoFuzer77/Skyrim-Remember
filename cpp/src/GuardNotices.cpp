#include "GuardNotices.h"

#include <format>

namespace SkyrimRemember {

void GuardNoticeBoard::Record(std::uint32_t guard, GuardReaction reaction) {
  if (reaction == GuardReaction::None) {
    return;
  }
  auto it = guards.find(guard);
  if (it == guards.end()) {
    if (reaction == GuardReaction::Unrecognized) {
      // Nothing was announced, so there is nothing to take back. A guard who
      // simply does not know the Player is not news.
      return;
    }
    it = guards.try_emplace(guard).first;
  }
  it->second.pending = reaction;
}

GuardNoticeSummary GuardNoticeBoard::Flush(Clock::time_point now) {
  GuardNoticeSummary summary;
  if (noticeShown && now - lastNotice < GuardNoticeTuning::groupingInterval) {
    return summary;
  }

  for (auto it = guards.begin(); it != guards.end();) {
    auto &state = it->second;
    if (state.pending != GuardReaction::None) {
      const auto sinceAnnounced = now - state.announcedAt;
      const auto announcedBefore = state.announced != GuardReaction::None;
      const auto repeated = state.pending == state.announced && sinceAnnounced < GuardNoticeTuning::repeatAfter;
      const auto retractsNothing = state.pending == GuardReaction::Unrecognized && state.announced != GuardReaction::Recognized;
      if (repeated || retractsNothing) {
        state.pending = GuardReaction::None;
      } else if (!announcedBefore || sinceAnnounced >= GuardNoticeTuning::perGuardSpacing) {
        ++(state.pending == GuardReaction::Recognized ? summary.recognized : summary.unrecognized);
        state.announced = state.pending;
        state.announcedAt = now;
        state.pending = GuardReaction::None;
      }
      // Otherwise this guard spoke too recently: keep the verdict queued.
    }

    const auto forgotten = state.announced == GuardReaction::None || now - state.announcedAt >= GuardNoticeTuning::repeatAfter;
    if (state.pending == GuardReaction::None && forgotten) {
      it = guards.erase(it);
    } else {
      ++it;
    }
  }

  if (!summary.Empty()) {
    lastNotice = now;
    noticeShown = true;
  }
  return summary;
}

void GuardNoticeBoard::Reset() {
  guards.clear();
  lastNotice = {};
  noticeShown = false;
}

std::string FormatGuardNotice(GuardReaction reaction, std::size_t count) {
  if (count == 0) {
    return {};
  }
  switch (reaction) {
  case GuardReaction::Recognized:
    return count == 1 ? std::string{"A guard recognizes you."} : std::format("{} guards recognize you.", count);
  case GuardReaction::Unrecognized:
    return count == 1 ? std::string{"A guard no longer recognizes you."} : std::format("{} guards no longer recognize you.", count);
  case GuardReaction::None:
    break;
  }
  return {};
}

} // namespace SkyrimRemember
