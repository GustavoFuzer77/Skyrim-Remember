#include "AppearanceMemoryService.h"
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
    SKSE::log::error("Game time unavailable: invalid GameDaysPassed={}", gameDays);
    return std::nullopt;
  }
  return static_cast<double>(gameDays);
}

void LogAppearance(const char *event, const NpcObservationContext &context, const PerceptionOutcome &outcome, const AppearanceResult &result) {
  SKSE::log::info("{}: observer={:08X} base={:08X} cell={:08X} worldspace={:08X} "
                  "location={:08X} persistent={} detection={} distance={} "
                  "appearanceMatched={} recognized={} linked={} learned={} faceVisible={} "
                  "outfitSimilarity={} confidence={} threshold={} items={} known={}",
                  event,
                  context.referenceFormId,
                  context.baseFormId,
                  context.cellFormId,
                  context.worldspaceFormId,
                  context.locationFormId,
                  context.persistentReference,
                  outcome.detectionLevel,
                  outcome.distance,
                  result.appearanceMatched != 0,
                  result.recognized != 0,
                  result.linkedByContinuity != 0,
                  result.learned != 0,
                  result.faceVisible != 0,
                  result.outfitSimilarity,
                  result.identityConfidence,
                  result.recognitionThreshold,
                  result.itemCount,
                  result.knownAppearances);
}
} // namespace

void AppearanceMemoryService::Configure(MemoryEngine *memoryEngine) {
  engine = memoryEngine;
}

void AppearanceMemoryService::SetSessionActive(bool value) {
  sessionActive = value;
}

void AppearanceMemoryService::Reset() {
  lastPlayerAppearance.reset();
  routineAppearance.reset();
  observerStates.clear();
  metrics = {};
  lastMetrics = Clock::now();
  transitionActive = false;
  transitionEvents = 0;
}

void AppearanceMemoryService::CaptureBaseline() {
  lastPlayerAppearance = Capture();
}

void AppearanceMemoryService::BeginObservationPass() {
  PruneObserverStates();
  routineAppearance.reset();
  ++metrics.passes;
}

CapturedAppearance AppearanceMemoryService::Capture() {
  const auto started = Clock::now();
  auto captured = CapturePlayerAppearance();
  metrics.capturing += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started);
  ++metrics.appearanceCaptures;
  return captured;
}

BridgeStatus AppearanceMemoryService::Register(std::uint32_t observer, double gameDays) {
  const auto started = Clock::now();
  const auto status = skyrim_remember_register(engine, observer, gameDays);
  metrics.bridging += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started);
  ++metrics.bridgeCalls;
  if (status != BridgeStatus::Created && status != BridgeStatus::Ok) {
    ++metrics.failures;
  }
  return status;
}

BridgeStatus AppearanceMemoryService::Query(std::uint32_t observer, const CapturedAppearance &appearance, bool visualContinuity, bool learn, double gameDays,
                                            AppearanceResult &result) {
  const auto &items = appearance.items;
  const auto *data = items.empty() ? nullptr : items.data();
  const auto faceVisible = appearance.faceVisible ? 1U : 0U;
  const auto continuity = visualContinuity ? 1U : 0U;
  const auto started = Clock::now();
  const auto status = learn ? skyrim_remember_observe_appearance(engine, observer, gameDays, data, items.size(), faceVisible, continuity, &result)
                            : skyrim_remember_evaluate_appearance(engine, observer, data, items.size(), faceVisible, continuity, &result);
  metrics.bridging += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started);
  ++metrics.bridgeCalls;
  if (status != BridgeStatus::Ok) {
    ++metrics.failures;
  }
  return status;
}

void AppearanceMemoryService::ReportMetrics() {
  const auto now = Clock::now();
  if (now - lastMetrics < ObservationTuning::metricsInterval) {
    return;
  }
  lastMetrics = now;
  if (metrics.bridgeCalls == 0) {
    return;
  }
  // A routine pass captures lazily and at most once, however many NPCs observe
  // the Player during it. Transitions reuse the pair captured by the equipment
  // batch and do not add one capture per observer either.
  SKSE::log::info("Appearance memory cost: passes={} routine={} transitions={} captures={} "
                  "captureUs={} bridgeCalls={} bridgeUs={} failures={} tracked={}",
                  metrics.passes,
                  metrics.routine,
                  metrics.transitions,
                  metrics.appearanceCaptures,
                  metrics.capturing.count(),
                  metrics.bridgeCalls,
                  metrics.bridging.count(),
                  metrics.failures,
                  observerStates.size());
  metrics = {};
}

bool AppearanceMemoryService::RecordRecognition(const NpcObservationContext &context, bool recognized) {
  auto &state = observerStates[context.referenceFormId];
  const auto changed = !state.known || state.recognized != recognized;
  state.known = true;
  state.recognized = recognized;
  state.lastObserved = Clock::now();
  return changed;
}

void AppearanceMemoryService::PruneObserverStates() {
  // Only the last verdict is forgotten here, never the memory in the Rust core:
  // an observer unseen this long meets the Player again as a new encounter.
  const auto now = Clock::now();
  std::erase_if(observerStates, [now](const auto &entry) {
    return now - entry.second.lastObserved > ObservationTuning::observerRetention;
  });
}

std::optional<IdentityObservation> AppearanceMemoryService::Observe(const NpcObservationContext &context, const PerceptionOutcome &outcome,
                                                                    bool appearanceChanged) {
  if (!sessionActive || !engine) {
    return std::nullopt;
  }
  if (appearanceChanged && transitionActive) {
    ++metrics.transitions;
    auto observation = ObserveTransition(context, outcome);
    ReportMetrics();
    return observation;
  }
  ++metrics.routine;
  auto observation = ObserveRoutine(context, outcome);
  ReportMetrics();
  return observation;
}

std::optional<IdentityObservation> AppearanceMemoryService::ObserveRoutine(const NpcObservationContext &context, const PerceptionOutcome &outcome) {
  const auto gameDays = CurrentGameDays();
  if (!gameDays) {
    return std::nullopt;
  }
  const auto observer = context.referenceFormId;
  const auto registered = Register(observer, *gameDays);
  if (registered != BridgeStatus::Created && registered != BridgeStatus::Ok) {
    SKSE::log::error("Observer registration failed: ref={:08X} status={}", observer, static_cast<std::uint32_t>(registered));
    return std::nullopt;
  }

  // Seeing an unchanged Player again is idempotent on the Rust side: the outfit
  // is only refreshed once its interval has really elapsed, so polling cannot
  // keep a known appearance eternally fresh.
  if (!routineAppearance) {
    routineAppearance = Capture();
  }
  AppearanceResult result{};
  const auto status = Query(observer, *routineAppearance, false, true, *gameDays, result);
  if (status != BridgeStatus::Ok) {
    SKSE::log::error("Observation failed: ref={:08X} status={}", observer, static_cast<std::uint32_t>(status));
    return std::nullopt;
  }

  // Log the events that mean something, not every tick of the scan.
  const auto recognitionChanged = RecordRecognition(context, result.recognized != 0);
  if (registered == BridgeStatus::Created || result.learned != 0 || recognitionChanged) {
    LogAppearance("Observation", context, outcome, result);
  }
  return IdentityObservation{result, registered == BridgeStatus::Created, recognitionChanged};
}

std::optional<IdentityObservation> AppearanceMemoryService::ObserveTransition(const NpcObservationContext &context, const PerceptionOutcome &outcome) {
  const auto gameDays = CurrentGameDays();
  if (!gameDays) {
    return std::nullopt;
  }
  const auto observer = context.referenceFormId;
  const auto registered = Register(observer, *gameDays);
  if (registered != BridgeStatus::Created && registered != BridgeStatus::Ok) {
    SKSE::log::error("Transition observer registration failed: ref={:08X} status={}", observer, static_cast<std::uint32_t>(registered));
    return std::nullopt;
  }

  // Did this NPC recognize the Player a moment ago, before the change? A pure
  // query: it must not learn or refresh anything.
  AppearanceResult before{};
  auto status = Query(observer, transitionPrevious, false, false, *gameDays, before);
  if (status != BridgeStatus::Ok) {
    SKSE::log::error("Previous appearance evaluation failed: ref={:08X} status={}", observer, static_cast<std::uint32_t>(status));
    return std::nullopt;
  }

  // An NPC watching a recognized Player keeps the identity across the
  // change, so the new outfit is learned as the Player's.
  AppearanceResult after{};
  status = Query(observer, transitionCurrent, before.recognized != 0, true, *gameDays, after);
  if (status != BridgeStatus::Ok) {
    SKSE::log::error("Changed appearance observation failed: ref={:08X} "
                     "status={}",
                     observer,
                     static_cast<std::uint32_t>(status));
    return std::nullopt;
  }

  const auto recognitionChanged = RecordRecognition(context, after.recognized != 0);
  SKSE::log::info("Appearance transition: observer={:08X} batchedEvents={} "
                  "previousRecognized={}",
                  observer,
                  transitionEvents,
                  before.recognized != 0);
  LogAppearance("Appearance transition result", context, outcome, after);
  return IdentityObservation{after, registered == BridgeStatus::Created, recognitionChanged};
}

bool AppearanceMemoryService::BeginAppearanceTransition(std::uint64_t batchedEvents) {
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

void AppearanceMemoryService::EndAppearanceTransition() {
  transitionActive = false;
  transitionEvents = 0;
  transitionPrevious = {};
  transitionCurrent = {};
}

} // namespace SkyrimRemember
