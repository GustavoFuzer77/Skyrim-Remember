#pragma once

#include "Appearance.h"
#include "NpcObservation.h"
#include "RustBridge.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace SkyrimRemember {

// The identity verdict shared by every NPC profile. Profile-specific reaction
// adapters consume this result without needing to know how appearance capture
// or recognition works.
struct IdentityObservation {
  AppearanceResult appearance;
  bool newObserver = false;
  // The verdict differs from this observer's previous one, or there was none:
  // a first sighting, the first observation after a load, or a new encounter
  // after the observer went unseen long enough to be pruned.
  bool recognitionChanged = false;
};

// Learns and evaluates what the Player looks like for every observing NPC.
//
// This is the only gameplay-side component that talks to the appearance bridge.
// It deliberately knows nothing about guards, followers, citizens, merchants,
// or what any of them should do with the recognition result.
class AppearanceMemoryService {
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

  // Invalidates the lazy routine capture. The first NPC perceived during this
  // pass captures the Player once; every other observer shares that snapshot.
  void BeginObservationPass();

  // One NPC perceived the Player. Returns no result when the session or bridge
  // is unavailable.
  std::optional<IdentityObservation> Observe(const NpcObservationContext &context, const PerceptionOutcome &outcome, bool appearanceChanged);

  // The equipment batch settled on a final new outfit. Returns false when
  // nothing actually changed, in which case no sweep is worth running.
  bool BeginAppearanceTransition(std::uint64_t batchedEvents);
  void EndAppearanceTransition();

private:
  using Clock = std::chrono::steady_clock;

  struct ObserverRecognitionState {
    bool recognized = false;
    bool known = false;
    Clock::time_point lastObserved;
  };

  // What the memory half of one observation costs, aggregated per interval.
  //
  // The service already times this whole block as "reportUs"; this splits it
  // into the part that reads the game and the part that crosses the bridge, so
  // a regression names its own cause.
  struct AdapterMetrics {
    std::uint64_t passes = 0;
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

  BridgeStatus Register(std::uint32_t observer, double gameDays);

  BridgeStatus Query(std::uint32_t observer, const CapturedAppearance &appearance, bool visualContinuity, bool learn, double gameDays,
                     AppearanceResult &result);

  std::optional<IdentityObservation> ObserveRoutine(const NpcObservationContext &context, const PerceptionOutcome &outcome);

  std::optional<IdentityObservation> ObserveTransition(const NpcObservationContext &context, const PerceptionOutcome &outcome);

  bool RecordRecognition(const NpcObservationContext &context, bool recognized);

  void PruneObserverStates();

  MemoryEngine *engine = nullptr;
  AdapterMetrics metrics;
  Clock::time_point lastMetrics;
  std::optional<CapturedAppearance> lastPlayerAppearance;
  std::optional<CapturedAppearance> routineAppearance;
  std::unordered_map<std::uint32_t, ObserverRecognitionState> observerStates;
  CapturedAppearance transitionPrevious;
  CapturedAppearance transitionCurrent;
  std::uint64_t transitionEvents = 0;
  bool transitionActive = false;
  bool sessionActive = false;
};

} // namespace SkyrimRemember
