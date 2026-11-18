#include "DebugBoard.h"

#include <algorithm>

namespace SkyrimRemember {

void DebugBoard::Record(DebugRow row, Clock::time_point now) {
  const std::scoped_lock lock(mutex);
  const auto id = row.referenceFormId;
  entries[id] = Entry{std::move(row), now};
}

void DebugBoard::Prune(Clock::time_point now) {
  const std::scoped_lock lock(mutex);
  std::erase_if(entries, [now](const auto &entry) { return now - entry.second.lastSeen > rowRetention; });
}

void DebugBoard::Clear() {
  const std::scoped_lock lock(mutex);
  entries.clear();
}

void DebugBoard::SetStatus(const DebugStatus &value) {
  const std::scoped_lock lock(mutex);
  const auto attacks = status.attacksStarted;
  status = value;
  status.attacksStarted = attacks;
}

void DebugBoard::CountAttack() {
  const std::scoped_lock lock(mutex);
  ++status.attacksStarted;
}

std::vector<DebugRow> DebugBoard::Rows() const {
  std::vector<DebugRow> rows;
  {
    const std::scoped_lock lock(mutex);
    rows.reserve(entries.size());
    for (const auto &[id, entry] : entries) {
      rows.push_back(entry.row);
    }
  }
  std::ranges::sort(rows, [](const DebugRow &left, const DebugRow &right) {
    return left.distance != right.distance ? left.distance < right.distance : left.referenceFormId < right.referenceFormId;
  });
  return rows;
}

DebugStatus DebugBoard::Status() const {
  const std::scoped_lock lock(mutex);
  return status;
}

HostilitySettings DebugBoard::Hostility() const {
  const std::scoped_lock lock(mutex);
  return hostility;
}

void DebugBoard::SetHostility(const HostilitySettings &value) {
  const std::scoped_lock lock(mutex);
  hostility = value;
}

bool DebugBoard::OverlayVisible() const {
  const std::scoped_lock lock(mutex);
  return overlayVisible;
}

void DebugBoard::SetOverlayVisible(bool value) {
  const std::scoped_lock lock(mutex);
  overlayVisible = value;
}

} // namespace SkyrimRemember
