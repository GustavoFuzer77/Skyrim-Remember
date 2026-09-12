#include "GuardMemoryAdapter.h"
#include "ObservationTuning.h"
#include "PlayerAppearance.h"

#include <RE/C/Calendar.h>
#include <SKSE/SKSE.h>

#include <cmath>
#include <utility>

namespace SkyrimRemember {
namespace {
std::optional<double> CurrentGameDays() {
  const auto *calendar = RE::Calendar::GetSingleton();
  if (!calendar) {
    SKSE::log::error("Game time unavailable: Calendar singleton missing");
    return std::nullopt;
  }
  const auto gameDays = calendar->GetDaysPassed();
  if (!std::isfinite(gameDays) || gameDays < 0.0F) {
    SKSE::log::error("Game time unavailable: invalid GameDaysPassed={}",
                     gameDays);
    return std::nullopt;
  }
  return static_cast<double>(gameDays);
}

void LogAppearance(const char *event, const NpcObservationContext &context,
                   const PerceptionOutcome &outcome,
                   const AppearanceResult &result) {
  SKSE::log::info(
      "{}: observer={:08X} base={:08X} cell={:08X} worldspace={:08X} "
      "location={:08X} persistent={} detection={} distance={} "
      "appearanceMatched={} recognized={} linked={} learned={} faceVisible={} "
      "outfitSimilarity={} confidence={} threshold={} items={} known={}",
      event, context.referenceFormId, context.baseFormId, context.cellFormId,
      context.worldspaceFormId, context.locationFormId,
      context.persistentReference, outcome.detectionLevel, outcome.distance,
      result.appearanceMatched != 0, result.recognized != 0,
      result.linkedByContinuity != 0, result.learned != 0,
      result.faceVisible != 0, result.outfitSimilarity,
      result.identityConfidence, result.recognitionThreshold, result.itemCount,
      result.knownAppearances);
}
} // namespace

void GuardMemoryAdapter::Configure(MemoryEngine *memoryEngine) {
  engine = memoryEngine;
}

void GuardMemoryAdapter::SetSessionActive(bool value) { sessionActive = value; }

void GuardMemoryAdapter::Reset() {
  lastPlayerAppearance.reset();
  logState.clear();
  metrics = {};
  lastMetrics = Clock::now();
  transitionActive = false;
  transitionEvents = 0;
}

void GuardMemoryAdapter::CaptureBaseline() {
  lastPlayerAppearance = Capture();
}

CapturedAppearance GuardMemoryAdapter::Capture() {
  const auto started = Clock::now();
  auto captured = CapturePlayerAppearance();
  metrics.capturing += std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - started);
  ++metrics.appearanceCaptures;
  return captured;
}

BridgeStatus GuardMemoryAdapter::Query(std::uint32_t observer,
                                       const CapturedAppearance &appearance,
                                       bool visualContinuity, bool learn,
                                       double gameDays,
                                       AppearanceResult &result) {
  const auto &items = appearance.items;
  const auto *data = items.empty() ? nullptr : items.data();
  const auto faceVisible = appearance.faceVisible ? 1U : 0U;
  const auto continuity = visualContinuity ? 1U : 0U;
  const auto started = Clock::now();
  const auto status =
      learn ? skyrim_remember_observe_appearance(engine, observer, gameDays,
                                                 data, items.size(),
                                                 faceVisible, continuity,
                                                 &result)
            : skyrim_remember_evaluate_appearance(engine, observer, data,
                                                  items.size(), faceVisible,
                                                  continuity, &result);
  metrics.bridging += std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - started);
  ++metrics.bridgeCalls;
  if (status != BridgeStatus::Ok) {
    ++metrics.failures;
  }
  return status;
}

void GuardMemoryAdapter::ReportMetrics() {
  const auto now = Clock::now();
  if (now - lastMetrics < ObservationTuning::metricsInterval) {
    return;
  }
  lastMetrics = now;
  if (metrics.bridgeCalls == 0) {
    return;
  }
  // captures tracking routine one-for-one is expected today: the Player is
  // captured once per observer rather than once per pass.
  SKSE::log::info(
      "Memory adapter cost: routine={} transitions={} captures={} "
      "captureUs={} bridgeCalls={} bridgeUs={} failures={} tracked={}",
      metrics.routine, metrics.transitions, metrics.appearanceCaptures,
      metrics.capturing.count(), metrics.bridgeCalls, metrics.bridging.count(),
      metrics.failures, logState.size());
  metrics = {};
}

bool GuardMemoryAdapter::RecognitionChanged(std::uint32_t observer,
                                            bool recognized) {
  auto &state = logState[observer];
  const auto changed = !state.known || state.recognized != recognized;
  state.known = true;
  state.recognized = recognized;
  return changed;
}

void GuardMemoryAdapter::OnObserved(const NpcObservationContext &context,
                                    const PerceptionOutcome &outcome,
                                    NpcObservationService::Trigger trigger) {
  if (!sessionActive || !engine) {
    return;
  }
  if (trigger == NpcObservationService::Trigger::AppearanceChanged &&
      transitionActive) {
    ++metrics.transitions;
    ObserveTransition(context, outcome);
    ReportMetrics();
    return;
  }
  ++metrics.routine;
  ObserveRoutine(context, outcome);
  ReportMetrics();
}

void GuardMemoryAdapter::ObserveRoutine(const NpcObservationContext &context,
                                        const PerceptionOutcome &outcome) {
  const auto gameDays = CurrentGameDays();
  if (!gameDays) {
    return;
  }
  const auto observer = context.referenceFormId;
  const auto registered =
      skyrim_remember_register(engine, observer, *gameDays);
  if (registered != BridgeStatus::Created && registered != BridgeStatus::Ok) {
    SKSE::log::error("Observer registration failed: ref={:08X} status={}",
                     observer, static_cast<std::uint32_t>(registered));
    return;
  }

  // Seeing an unchanged Player again is idempotent on the Rust side: the outfit
  // is only refreshed once its interval has really elapsed, so polling cannot
  // keep a known appearance eternally fresh.
  const auto appearance = Capture();
  AppearanceResult result{};
  const auto status =
      Query(observer, appearance, false, true, *gameDays, result);
  if (status != BridgeStatus::Ok) {
    SKSE::log::error("Observation failed: ref={:08X} status={}", observer,
                     static_cast<std::uint32_t>(status));
    return;
  }

  // Log the events that mean something, not every tick of the scan.
  const auto recognitionChanged =
      RecognitionChanged(observer, result.recognized != 0);
  if (registered == BridgeStatus::Created || result.learned != 0 ||
      recognitionChanged) {
    LogAppearance("Observation", context, outcome, result);
  }
}

void GuardMemoryAdapter::ObserveTransition(const NpcObservationContext &context,
                                           const PerceptionOutcome &outcome) {
  const auto gameDays = CurrentGameDays();
  if (!gameDays) {
    return;
  }
  const auto observer = context.referenceFormId;
  const auto registered =
      skyrim_remember_register(engine, observer, *gameDays);
  if (registered != BridgeStatus::Created && registered != BridgeStatus::Ok) {
    SKSE::log::error(
        "Transition observer registration failed: ref={:08X} status={}",
        observer, static_cast<std::uint32_t>(registered));
    return;
  }

  // Did this guard recognize the Player a moment ago, before the change? A pure
  // query: it must not learn or refresh anything.
  AppearanceResult before{};
  auto status = Query(observer, transitionPrevious, false, false, *gameDays,
                      before);
  if (status != BridgeStatus::Ok) {
    SKSE::log::error(
        "Previous appearance evaluation failed: ref={:08X} status={}", observer,
        static_cast<std::uint32_t>(status));
    return;
  }

  // A guard that was watching a recognized Player keeps the identity across the
  // change, so the new outfit is learned as the Player's.
  AppearanceResult after{};
  status = Query(observer, transitionCurrent, before.recognized != 0, true,
                 *gameDays, after);
  if (status != BridgeStatus::Ok) {
    SKSE::log::error("Changed appearance observation failed: ref={:08X} "
                     "status={}",
                     observer, static_cast<std::uint32_t>(status));
    return;
  }

  RecognitionChanged(observer, after.recognized != 0);
  SKSE::log::info("Appearance transition: observer={:08X} batchedEvents={} "
                  "previousRecognized={}",
                  observer, transitionEvents, before.recognized != 0);
  LogAppearance("Appearance transition result", context, outcome, after);
}

bool GuardMemoryAdapter::BeginAppearanceTransition(
    std::uint64_t batchedEvents) {
  if (!sessionActive || !engine) {
    return false;
  }
  auto current = Capture();
  if (!lastPlayerAppearance) {
    // No baseline yet: adopt this one instead of inventing a transition.
    lastPlayerAppearance = std::move(current);
    return false;
  }
  auto previous = std::move(*lastPlayerAppearance);
  lastPlayerAppearance = current;
  if (previous == current) {
    return false;
  }

  transitionPrevious = std::move(previous);
  transitionCurrent = std::move(current);
  transitionEvents = batchedEvents;
  transitionActive = true;
  return true;
}

void GuardMemoryAdapter::EndAppearanceTransition() {
  transitionActive = false;
  transitionEvents = 0;
  transitionPrevious = {};
  transitionCurrent = {};
}

} // namespace SkyrimRemember
