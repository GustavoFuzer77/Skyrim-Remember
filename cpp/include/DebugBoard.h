#pragma once

#include "GuardHostility.h"
#include "MemoryTuning.h"
#include "NpcProfile.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// What the in-game debug panel shows, and the knobs it turns.
//
// The panel draws on the render thread; everything here is filled on the main
// thread, from data already read out of the game. So the panel never touches a
// Skyrim object, and this header stays game-free and testable.
namespace SkyrimRemember {

// One NPC that perceived the Player recently.
struct DebugRow {
  std::uint32_t referenceFormId = 0;
  std::string name;
  NpcProfile profile = NpcProfile::Generic;
  float distance = 0.0F;
  bool recognized = false;
  float outfitSimilarity = 0.0F;
  float recognitionThreshold = 0.0F;
  std::array<float, axisCapacity> axes{};
  // Only meaningful for guards.
  HostilityVerdict hostility = HostilityVerdict::Calm;
  bool inCombat = false;
};

struct DebugStatus {
  bool playing = false;
  std::uint32_t impressions = 0;
  std::uint32_t references = 0;
  std::uint32_t boundReferences = 0;
  std::uint64_t attacksStarted = 0;
};

// Thread-safe snapshot shared between the game and the panel.
class DebugBoard {
public:
  using Clock = std::chrono::steady_clock;

  // How long an NPC stays listed after it last perceived the Player.
  static constexpr std::chrono::seconds rowRetention{20};

  // Main thread.
  void Record(DebugRow row, Clock::time_point now);
  void Prune(Clock::time_point now);
  void Clear();
  void SetStatus(const DebugStatus &value);
  void CountAttack();

  // Any thread. Rows come sorted nearest first.
  [[nodiscard]] std::vector<DebugRow> Rows() const;
  [[nodiscard]] DebugStatus Status() const;

  // Written by the panel, read by the game.
  [[nodiscard]] HostilitySettings Hostility() const;
  void SetHostility(const HostilitySettings &value);
  [[nodiscard]] bool OverlayVisible() const;
  void SetOverlayVisible(bool value);

private:
  struct Entry {
    DebugRow row;
    Clock::time_point lastSeen;
  };

  mutable std::mutex mutex;
  std::unordered_map<std::uint32_t, Entry> entries;
  DebugStatus status;
  HostilitySettings hostility;
  bool overlayVisible = true;
};

} // namespace SkyrimRemember
