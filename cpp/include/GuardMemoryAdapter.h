#pragma once

#include "Appearance.h"
#include "NpcObservation.h"
#include "NpcObservationService.h"
#include "RustBridge.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace SkyrimRemember {

// Turns a perceived observation into a call on the Rust memory core.
//
// This is the only place that talks to the bridge, and it is deliberately
// guard-shaped: when Follower or Citizen grow behaviour, they get an adapter of
// their own rather than an extra branch in here.
class GuardMemoryAdapter {
public:
  void Configure(MemoryEngine *memoryEngine);

  // Gameplay is running and the store is usable.
  void SetSessionActive(bool value);

  // Load, revert, or new game: forget the baseline and the per-observer log
  // state. Nothing here is persisted.
  void Reset();

  // Records what the Player looks like now, so the next equipment change has a
  // "before" to compare against.
  void CaptureBaseline();

  // One NPC perceived the Player.
  void OnObserved(const NpcObservationContext &context,
                  const PerceptionOutcome &outcome,
                  NpcObservationService::Trigger trigger);

  // The equipment batch settled on a final new outfit. Returns false when
  // nothing actually changed, in which case no sweep is worth running.
  bool BeginAppearanceTransition(std::uint64_t batchedEvents);
  void EndAppearanceTransition();

private:
  using Clock = std::chrono::steady_clock;

  struct ObserverLogState {
    bool recognized = false;
    bool known = false;
  };

  // What the memory half of one observation costs, aggregated per interval.
  //
  // The service already times this whole block as "reportUs"; this splits it
  // into the part that reads the game and the part that crosses the bridge, so
  // a regression names its own cause.
  struct AdapterMetrics {
    std::uint64_t routine = 0;
    std::uint64_t transitions = 0;
    std::uint64_t appearanceCaptures = 0;
    std::uint64_t bridgeCalls = 0;
    std::uint64_t failures = 0;
    std::chrono::microseconds capturing{0};
    std::chrono::microseconds bridging{0};
  };

  void ReportMetrics();

  CapturedAppearance Capture();

  BridgeStatus Query(std::uint32_t observer,
                     const CapturedAppearance &appearance,
                     bool visualContinuity, bool learn, double gameDays,
                     AppearanceResult &result);

  void ObserveRoutine(const NpcObservationContext &context,
                      const PerceptionOutcome &outcome);

  void ObserveTransition(const NpcObservationContext &context,
                         const PerceptionOutcome &outcome);

  bool RecognitionChanged(std::uint32_t observer, bool recognized);

  MemoryEngine *engine = nullptr;
  AdapterMetrics metrics;
  Clock::time_point lastMetrics{};
  std::optional<CapturedAppearance> lastPlayerAppearance;
  std::unordered_map<std::uint32_t, ObserverLogState> logState;
  CapturedAppearance transitionPrevious;
  CapturedAppearance transitionCurrent;
  std::uint64_t transitionEvents = 0;
  bool transitionActive = false;
  bool sessionActive = false;
};

} // namespace SkyrimRemember
