#include "NpcObservationService.h"
#include "ObservationTuning.h"

#include <RE/A/Actor.h>
#include <RE/B/BSContainer.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/P/ProcessLists.h>
#include <SKSE/SKSE.h>

#include <utility>

namespace SkyrimRemember {
namespace {
std::size_t ProfileIndex(NpcProfile profile) {
  const auto index = static_cast<std::size_t>(profile);
  return index < npcProfileCount ? index : npcProfileCount - 1;
}
} // namespace

void NpcObservationService::Configure(const SKSE::TaskInterface *taskInterface,
                                      Observer onObserved) {
  tasks = taskInterface;
  observer = std::move(onObserved);
  const auto now = Clock::now();
  lastScan = now;
  lastMetrics = now;
  if (!armed) {
    armed = true;
    Arm();
  }
}

void NpcObservationService::SetActive(bool value) { active = value; }

void NpcObservationService::Reset() {
  ++generation;
  pending.clear();
  observers.clear();
  metrics = {};
  const auto now = Clock::now();
  lastScan = now;
  lastMetrics = now;
  // The abandoned slice will notice the generation and stop; this restarts the
  // chain so a reset never leaves the scan without an owner.
  Arm();
}

void NpcObservationService::Arm() {
  if (!tasks) {
    return;
  }
  tasks->AddTask([this, scheduled = generation] {
    if (generation != scheduled) {
      // A Reset armed a fresh chain; this one is stale.
      return;
    }
    try {
      Tick();
    } catch (...) {
      SKSE::log::error("Observation tick failed with an exception");
    }
    // Rescheduling is separate from the work, so one bad slice cannot end the
    // chain and a failure to reschedule cannot go unnoticed.
    try {
      Arm();
    } catch (...) {
      SKSE::log::critical("Observation scan stopped: could not reschedule");
    }
  });
}

void NpcObservationService::Tick() {
  if (!active) {
    return;
  }
  const auto now = Clock::now();
  if (now - lastScan >= ObservationTuning::scanInterval) {
    lastScan = now;
    RunSlice(now);
  }
  ReportMetrics(now);
}

void NpcObservationService::RefreshPending() {
  auto *processLists = RE::ProcessLists::GetSingleton();
  if (!processLists) {
    return;
  }
  pending.clear();
  // Enumerating is cheap and gives a stable snapshot to work through across
  // slices. Only handles are kept.
  processLists->ForEachHighActor([this](RE::Actor &actor) {
    pending.push_back(actor.GetHandle());
    return RE::BSContainer::ForEachResult::kContinue;
  });
  ++metrics.refreshes;
  metrics.highActors = pending.size();
}

bool NpcObservationService::Examine(RE::Actor &actor,
                                    RE::PlayerCharacter &player, TimePoint now,
                                    Trigger trigger, bool respectCooldown) {
  ++metrics.examined;
  const auto profile = ClassifyNpc(actor);
  ++metrics.byProfile[ProfileIndex(profile)];
  if (!IsProfileEnabled(profile)) {
    // Classified and counted, but inert: a disabled profile must not spend the
    // budget that pays for detection and line of sight.
    return false;
  }
  ++metrics.enabled;

  const auto referenceFormId = actor.GetFormID();
  auto &state = observers[referenceFormId];
  if (respectCooldown && state.lastExamined != TimePoint{} &&
      now - state.lastExamined < ObservationTuning::observerCooldown) {
    ++metrics.cooledDown;
    // Cooled down still counts as budget: the lookup is what we are pacing.
    return true;
  }
  state.lastExamined = now;

  const auto outcome = PerceivePlayer(actor, player);
  if (outcome.detectionLevel > 0) {
    ++metrics.detected;
  }
  if (!outcome.Perceived()) {
    return true;
  }
  ++metrics.observed;
  if (observer) {
    observer(DescribeObservation(actor, profile), outcome, trigger);
  }
  return true;
}

void NpcObservationService::RunSlice(TimePoint now) {
  auto *player = RE::PlayerCharacter::GetSingleton();
  if (!player || !observer) {
    return;
  }
  const auto started = Clock::now();
  ++metrics.slices;
  if (pending.empty()) {
    RefreshPending();
  }

  auto budget = ObservationTuning::actorBudgetPerScan;
  while (budget > 0 && !pending.empty()) {
    const auto handle = pending.back();
    pending.pop_back();
    const auto actor = handle.get();
    if (!actor) {
      continue;
    }
    if (Examine(*actor, *player, now, Trigger::Routine, true)) {
      --budget;
    }
  }

  metrics.backlog = pending.size();
  metrics.duration += std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - started);
}

void NpcObservationService::ScanNow(Trigger trigger) {
  auto *player = RE::PlayerCharacter::GetSingleton();
  auto *processLists = RE::ProcessLists::GetSingleton();
  if (!active || !player || !processLists || !observer) {
    return;
  }
  const auto started = Clock::now();
  std::size_t reached = 0;
  processLists->ForEachHighActor([&](RE::Actor &actor) {
    if (Examine(actor, *player, started, trigger, false)) {
      ++reached;
    }
    return RE::BSContainer::ForEachResult::kContinue;
  });
  const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - started);
  SKSE::log::info("Immediate observation sweep: trigger={} candidates={} "
                  "durationUs={}",
                  trigger == Trigger::AppearanceChanged ? "appearanceChanged"
                                                        : "routine",
                  reached, duration.count());
}

void NpcObservationService::PruneObservers(TimePoint now) {
  std::erase_if(observers, [now](const auto &entry) {
    return now - entry.second.lastExamined > ObservationTuning::observerRetention;
  });
}

void NpcObservationService::ReportMetrics(TimePoint now) {
  if (now - lastMetrics < ObservationTuning::metricsInterval) {
    return;
  }
  lastMetrics = now;
  PruneObservers(now);
  if (metrics.slices == 0) {
    return;
  }
  // One aggregate line per interval. Never one line per actor.
  SKSE::log::info(
      "Observation metrics: slices={} refreshes={} highActors={} examined={} "
      "enabled={} cooledDown={} detected={} observed={} backlog={} "
      "totalUs={} trackedObservers={} guard={} follower={} citizen={} "
      "merchant={} generic={}",
      metrics.slices, metrics.refreshes, metrics.highActors, metrics.examined,
      metrics.enabled, metrics.cooledDown, metrics.detected, metrics.observed,
      metrics.backlog, metrics.duration.count(), observers.size(),
      metrics.byProfile[ProfileIndex(NpcProfile::Guard)],
      metrics.byProfile[ProfileIndex(NpcProfile::Follower)],
      metrics.byProfile[ProfileIndex(NpcProfile::Citizen)],
      metrics.byProfile[ProfileIndex(NpcProfile::Merchant)],
      metrics.byProfile[ProfileIndex(NpcProfile::Generic)]);
  metrics = {};
}

} // namespace SkyrimRemember
