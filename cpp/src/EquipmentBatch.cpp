#include "EquipmentBatch.h"

#include <RE/U/UI.h>
#include <SKSE/SKSE.h>

namespace SkyrimRemember {
namespace {
bool ItemMenuIsOpen() {
  const auto *ui = RE::UI::GetSingleton();
  return ui && ui->IsItemMenuOpen();
}
} // namespace

void EquipmentBatch::Configure(const SKSE::TaskInterface *taskInterface,
                               Flush onFlush) {
  tasks = taskInterface;
  flush = std::move(onFlush);
}

void EquipmentBatch::Reset() {
  ++generation;
  pendingEvents = 0;
  state = State::Idle;
}

void EquipmentBatch::RecordChange() {
  ++pendingEvents;
  if (state == State::Idle) {
    Schedule();
  }
}

void EquipmentBatch::MenuClosed() {
  if (state == State::Deferred) {
    Schedule();
  }
}

void EquipmentBatch::Schedule() {
  
  if (!tasks) {
    return;
  }

  if (ItemMenuIsOpen()) {
    state = State::Deferred;
    return;
  }

  state = State::Queued;
  
  tasks->AddTask([this, scheduled = generation] {
    if (generation != scheduled) {
      // A Reset ran between scheduling and now; its state wins.
      return;
    }
    if (ItemMenuIsOpen()) {
      // Another menu opened in the meantime. Wait for it instead of dropping
      // the batch: MenuClosed reschedules from here.
      state = State::Deferred;
      return;
    }
    state = State::Idle;
    const auto batched = pendingEvents;
    pendingEvents = 0;
    if (batched == 0 || !flush) {
      return;
    }
    try {
      flush(batched);
    } catch (...) {
      SKSE::log::error("Batched equipment update failed with an exception");
    }
  });
}

} // namespace SkyrimRemember
