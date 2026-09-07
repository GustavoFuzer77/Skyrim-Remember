#pragma once

#include "NpcObservation.h"
#include "NpcProfile.h"

#include <RE/B/BSPointerHandle.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace SKSE {
class TaskInterface;
}

namespace SkyrimRemember {

// Finds the NPCs that can currently see the Player and reports each one once.
//
// Observation starts at the NPC, never at what the Player is aiming at.
//
// The scan is paced by a timer thread whose *only* job is to hand one task to
// the main thread per interval. That thread never touches a Skyrim object;
// SKSE::TaskInterface::AddTask is the designated thread-safe way to move work
// onto the main thread, and every line of actual work runs there.
//
// It must not be a task that re-schedules itself: SKSE can drain its queue
// until the queue is empty, so a self-re-adding task never lets the frame end
// and the game freezes on the main menu. That mistake was made once here.
//
// Cost is bounded two ways: the cheap enumeration of high actors happens once
// per pass, and only a fixed budget of *enabled* actors pays for the expensive
// detection and line-of-sight tests per slice. What does not fit is carried to
// the next slice rather than dropped.
//
// The instance must outlive every task it schedules; the generation counter
// makes a Reset abandon the chain already in flight.
class NpcObservationService {
public:
  // Routine is the periodic sweep. AppearanceChanged is the discrete event
  // raised when the Player finishes changing outfit, which sweeps every
  // watcher at once and ignores both the budget and the cooldown.
  enum class Trigger : std::uint32_t { Routine, AppearanceChanged };

  using Observer = std::function<void(const NpcObservationContext &,
                                      const PerceptionOutcome &, Trigger)>;

  void Configure(const SKSE::TaskInterface *taskInterface, Observer onObserved);

  // Gameplay is running and observations may be reported.
  void SetActive(bool value);

  // Load, revert, or new game: drop the cursor, the cooldowns and the metrics,
  // and abandon any slice already scheduled.
  void Reset();

  // An immediate, unbudgeted sweep of every watcher. Costs one full pass, so it
  // belongs to discrete events only, never to polling.
  void ScanNow(Trigger trigger);

private:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  struct ObserverState {
    TimePoint lastExamined{};
  };

  struct ScanMetrics {
    std::uint64_t slices = 0;
    std::uint64_t refreshes = 0;
    std::uint64_t highActors = 0;
    std::uint64_t examined = 0;
    std::uint64_t enabled = 0;
    std::uint64_t cooledDown = 0;
    std::uint64_t detected = 0;
    std::uint64_t observed = 0;
    std::uint64_t backlog = 0;
    std::chrono::microseconds duration{0};
    std::array<std::uint64_t, npcProfileCount> byProfile{};
  };

  void Start();
  void Tick();
  void RunSlice(TimePoint now);
  void RefreshPending();
  void ReportMetrics(TimePoint now);
  void PruneObservers(TimePoint now);
  bool Examine(RE::Actor &actor, RE::PlayerCharacter &player, TimePoint now,
               Trigger trigger, bool respectCooldown);

  const SKSE::TaskInterface *tasks = nullptr;
  Observer observer;
  // Handles, never Actor*: a handle that has gone stale simply resolves to
  // null on the next slice instead of dangling.
  std::vector<RE::ActorHandle> pending;
  std::unordered_map<std::uint32_t, ObserverState> observers;
  ScanMetrics metrics;
  TimePoint lastScan{};
  TimePoint lastMetrics{};
  // Read by the timer thread, written by the main thread.
  std::atomic<bool> active{false};
  // Declared last on purpose: its destructor stops and joins the thread before
  // any member the thread might still be reading is destroyed.
  std::jthread timer;
};

} // namespace SkyrimRemember
