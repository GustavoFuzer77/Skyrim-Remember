#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

// Game-free on purpose: the notice policy is plain logic and BridgeSmoke tests
// it without Skyrim.
namespace SkyrimRemember {

enum class GuardReaction { None, Recognized, Unrecognized };

namespace GuardNoticeTuning {

// Shortest real time between two HUD notices. Everything decided in between is
// grouped into the next one, so a patrol recognizing the Player reads as one
// line instead of five.
inline constexpr std::chrono::seconds groupingInterval{4};

// Shortest real time between two notices about the same guard. An outfit match
// hovering at the threshold would otherwise alternate on every scan.
inline constexpr std::chrono::seconds perGuardSpacing{30};

// How long a guard's last notice is remembered. Within it, the same verdict is
// not news; after it, the next encounter is announced again.
inline constexpr std::chrono::minutes repeatAfter{5};

} // namespace GuardNoticeTuning

struct GuardNoticeSummary {
  std::size_t recognized = 0;
  std::size_t unrecognized = 0;

  [[nodiscard]] bool Empty() const {
    return recognized == 0 && unrecognized == 0;
  }
};

// Turns per-guard reactions into few, grouped HUD notices.
//
// It never decides whether a guard recognizes the Player — each guard's memory
// and threshold live in the Rust core. It only decides whether telling the
// Player about it now is news or spam.
class GuardNoticeBoard {
public:
  using Clock = std::chrono::steady_clock;

  // Queues the latest reaction of one guard. A newer reaction of the same guard
  // replaces an unannounced older one, so flicker cancels itself out.
  void Record(std::uint32_t guard, GuardReaction reaction);

  // What should be shown now. Empty while the grouping interval is still
  // running; whatever is held back stays queued for a later flush.
  GuardNoticeSummary Flush(Clock::time_point now);

  void Reset();

  [[nodiscard]] std::size_t Tracked() const {
    return guards.size();
  }

private:
  struct GuardState {
    GuardReaction announced = GuardReaction::None;
    GuardReaction pending = GuardReaction::None;
    Clock::time_point announcedAt;
  };

  std::unordered_map<std::uint32_t, GuardState> guards;
  Clock::time_point lastNotice;
  bool noticeShown = false;
};

// The HUD line for count guards sharing one reaction; empty when count is zero.
std::string FormatGuardNotice(GuardReaction reaction, std::size_t count);

} // namespace SkyrimRemember
