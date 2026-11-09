#include "NpcObservationService.h"
#include "ObservationTuning.h"

#include <RE/A/Actor.h>
#include <RE/B/BSContainer.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/P/ProcessLists.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <utility>

namespace SkyrimRemember {
namespace {
std::size_t ProfileIndex(NpcProfile profile) {
  const auto index = static_cast<std::size_t>(profile);
  return index < npcProfileCount ? index : npcProfileCount - 1;
}

using Microseconds = std::chrono::microseconds;

Microseconds Since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<Microseconds>(std::chrono::steady_clock::now() - start);
}
} // namespace

void NpcObservationService::Configure(const SKSE::TaskInterface *taskInterface, PassStarted onPassStarted, Observer onObserved) {
  tasks = taskInterface;
  passStarted = std::move(onPassStarted);
  observer = std::move(onObserved);
  const auto now = Clock::now();
  lastScan = now;
  lastMetrics = now;
  reportedAt = now;
  Start();
}

void NpcObservationService::SetActive(bool value) {
  active.store(value, std::memory_order_release);
}

void NpcObservationService::Reset() {
  pending.clear();
  observers.clear();
  metrics = {};
  const auto now = Clock::now();
  lastScan = now;
  lastMetrics = now;
  reportedAt = now;
  // A task queued just before this runs harmlessly afterwards: it finds the
  // cursor and the cooldowns already cleared and simply starts a fresh slice.
}

void NpcObservationService::Start() {
  if (!tasks || timer.joinable()) {
    return;
  }
  timer = std::jthread([this](const std::stop_token &stop) {
    // Nothing in this loop touches a Skyrim object. It sleeps, and it hands a
    // task to the main thread. Waking in small steps so that shutdown does not
    // have to wait out a whole interval.
    constexpr auto step = std::chrono::milliseconds{50};
    while (!stop.stop_requested()) {
      for (auto waited = std::chrono::milliseconds::zero(); waited < ObservationTuning::scanInterval && !stop.stop_requested(); waited += step) {
        std::this_thread::sleep_for(step);
      }
      if (stop.stop_requested()) {
        return;
      }
      if (!active.load(std::memory_order_acquire)) {
        continue;
      }
      tasks->AddTask([this] {
        try {
          ++metrics.tasks;
          Tick();
        } catch (...) {
          SKSE::log::error("Observation tick failed with an exception");
        }
      });
    }
  });
}

void NpcObservationService::Tick() {
  if (!active.load(std::memory_order_acquire)) {
    return;
  }
  const auto now = Clock::now();
  // The timer already paces this. The check only guards against two tasks
  // arriving in the same frame after a stall.
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

bool NpcObservationService::Examine(RE::Actor &actor, RE::PlayerCharacter &player, TimePoint now, Trigger trigger, bool respectCooldown) {
  ++metrics.examined;
  const auto classifyStarted = Clock::now();
  const auto profile = ClassifyNpc(actor);
  metrics.classifying += Since(classifyStarted);
  if (!profile) {
    return false;
  }
  ++metrics.byProfile[ProfileIndex(*profile)];
  if (!IsProfileEnabled(*profile)) {
    // Classified and counted, but inert: a disabled profile must not spend the
    // budget that pays for detection and line of sight.
    return false;
  }
  ++metrics.enabled;

  const auto referenceFormId = actor.GetFormID();
  auto &state = observers[referenceFormId];
  if (respectCooldown && state.lastExamined != TimePoint{} && now - state.lastExamined < ObservationTuning::observerCooldown) {
    ++metrics.cooledDown;
    // Cooled down still counts as budget: the lookup is what we are pacing.
    return true;
  }
  state.lastExamined = now;

  // The two expensive Skyrim calls live in here: detection, and the line-of-
  // sight ray cast. Timed apart from everything else because they are the most
  // likely source of a spike.
  const auto perceiveStarted = Clock::now();
  const auto outcome = PerceivePlayer(actor, player);
  metrics.perceiving += Since(perceiveStarted);
  if (outcome.detectionLevel > 0) {
    ++metrics.detected;
  }
  if (!outcome.Perceived()) {
    return true;
  }
  ++metrics.observed;
  if (observer) {
    // Everything downstream: reading the context, capturing the Player's
    // appearance, and the calls into the memory store.
    const auto reportStarted = Clock::now();
    observer(actor, DescribeObservation(actor, *profile), outcome, trigger);
    metrics.reporting += Since(reportStarted);
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
  if (passStarted) {
    passStarted(Trigger::Routine);
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
  const auto elapsed = Since(started);
  metrics.duration += elapsed;
  metrics.peakSlice = std::max(metrics.peakSlice, elapsed);
  if (elapsed > ObservationTuning::slowSliceThreshold) {
    // Reported the moment it happens rather than waiting for the aggregate, so
    // a stutter can be matched against what was on screen.
    ++metrics.slowSlices;
    SKSE::log::warn("Slow observation slice: durationUs={} highActors={} "
                    "examined={} enabled={} observed={} backlog={}",
                    elapsed.count(),
                    metrics.highActors,
                    metrics.examined,
                    metrics.enabled,
                    metrics.observed,
                    pending.size());
  }
}

void NpcObservationService::ScanNow(Trigger trigger) {
  auto *player = RE::PlayerCharacter::GetSingleton();
  auto *processLists = RE::ProcessLists::GetSingleton();
  if (!active || !player || !processLists || !observer) {
    return;
  }
  const auto started = Clock::now();
  std::size_t reached = 0;
  if (passStarted) {
    passStarted(trigger);
  }
  processLists->ForEachHighActor([&](RE::Actor &actor) {
    if (Examine(actor, *player, started, trigger, false)) {
      ++reached;
    }
    return RE::BSContainer::ForEachResult::kContinue;
  });
  const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started);
  SKSE::log::info("Immediate observation sweep: trigger={} candidates={} "
                  "durationUs={}",
                  trigger == Trigger::AppearanceChanged ? "appearanceChanged" : "routine",
                  reached,
                  duration.count());
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
  const auto windowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - reportedAt).count();
  const auto averageUs = static_cast<std::uint64_t>(metrics.duration.count()) / metrics.slices;
  reportedAt = now;

  // Two aggregate lines per interval. Never one line per actor.
  //
  // Cost first, because that is what a performance test reads. peakUs is the
  // number that matters: a spike is what a player feels, and it vanishes into
  // an average.
  SKSE::log::info("Observation cost: windowMs={} tasks={} slices={} totalUs={} avgUs={} "
                  "peakUs={} slowSlices={} classifyUs={} perceiveUs={} reportUs={}",
                  windowMs,
                  metrics.tasks,
                  metrics.slices,
                  metrics.duration.count(),
                  averageUs,
                  metrics.peakSlice.count(),
                  metrics.slowSlices,
                  metrics.classifying.count(),
                  metrics.perceiving.count(),
                  metrics.reporting.count());

  SKSE::log::info("Observation work: highActors={} examined={} enabled={} cooledDown={} "
                  "detected={} observed={} backlog={} trackedObservers={} guard={} "
                  "follower={} citizen={} merchant={} generic={}",
                  metrics.highActors,
                  metrics.examined,
                  metrics.enabled,
                  metrics.cooledDown,
                  metrics.detected,
                  metrics.observed,
                  metrics.backlog,
                  observers.size(),
                  metrics.byProfile[ProfileIndex(NpcProfile::Guard)],
                  metrics.byProfile[ProfileIndex(NpcProfile::Follower)],
                  metrics.byProfile[ProfileIndex(NpcProfile::Citizen)],
                  metrics.byProfile[ProfileIndex(NpcProfile::Merchant)],
                  metrics.byProfile[ProfileIndex(NpcProfile::Generic)]);
  metrics = {};
}

} // namespace SkyrimRemember
