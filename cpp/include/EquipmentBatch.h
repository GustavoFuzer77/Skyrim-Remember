#pragma once

#include <cstdint>
#include <functional>

namespace SKSE {
class TaskInterface;
}

namespace SkyrimRemember {

// Collapses a burst of equipment events into a single deferred update.
//
// Swapping a full outfit in the inventory menu produces one TESEquipEvent per
// piece, and the intermediate states are outfits the Player never actually
// wore. This type waits for the item menu to close and then reports the whole
// burst once.
//
//   Idle ---- a change arrives ----> Deferred (an item menu is open)
//        \                              |
//         `--- no menu open ---> Queued <' (menu closed)
//                                  |
//                                  `-- the task runs -> flush, back to Idle
//
// A queued task that finds an item menu open again returns to Deferred rather
// than dropping the change, so a pending batch always has an owner scheduled
// to retry it.
//
// Every method must run on Skyrim's main thread. Instances must outlive every
// task they schedule; the generation counter makes a Reset invalidate tasks
// already in flight.
class EquipmentBatch {
public:
  using Flush = std::function<void(std::uint64_t batchedEvents)>;

  void Configure(const SKSE::TaskInterface *taskInterface, Flush onFlush);

  // Drops every pending change and invalidates tasks already scheduled.
  void Reset();

  // One equipment event for the observed actor arrived.
  void RecordChange();

  // A menu closed; a batch deferred behind it may now be scheduled.
  void MenuClosed();

  bool HasPendingChange() const {
    return state != State::Idle;
  }

private:
  enum class State { Idle, Deferred, Queued };

  void Schedule();

  const SKSE::TaskInterface *tasks = nullptr;
  Flush flush;
  std::uint64_t generation = 0;
  std::uint64_t pendingEvents = 0;
  State state = State::Idle;
};

} // namespace SkyrimRemember
