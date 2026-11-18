#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_map>

// When a guard stops tolerating the Player and attacks.
//
// Game-free on purpose: the decision is plain logic over what the memory store
// already knows, and the tests cover it without Skyrim. Starting the fight is
// the adapter's job.
namespace SkyrimRemember {

// Live-tunable from the debug panel. Not persisted: every session starts from
// these defaults.
struct HostilitySettings {
  bool enabled = true;
  // Suspicion at or above this makes a recognizing guard attack. The shipped
  // bandit armor rule raises suspicion to 40 and hide armor to 15, so 35 means
  // "dressed as a bandit", not "dressed in furs".
  float suspicionThreshold = 35.0F;
  // Shortest real time between two attacks started by the same guard. Yielding
  // ends combat; without this the next scan would start it again at once.
  std::chrono::seconds cooldown{120};
};

enum class HostilityVerdict : std::uint32_t {
  // Nothing to do: the guard is calm.
  Calm = 0,
  // Start combat now.
  Attack,
  Disabled,
  // A guard who does not recognize the Player has nobody to be angry at.
  NotRecognized,
  // Already fighting, whoever started it.
  InCombat,
  // Suspicious enough, but attacked too recently.
  CoolingDown,
};

const char *HostilityVerdictName(HostilityVerdict verdict);

// Remembers when each guard last attacked, and decides whether one attacks now.
class GuardHostility {
public:
  using Clock = std::chrono::steady_clock;

  // An Attack verdict is recorded as an attack at now: the caller is expected
  // to start the fight.
  HostilityVerdict Decide(std::uint32_t guard, bool recognized, float suspicion, bool inCombat, Clock::time_point now, const HostilitySettings &settings);

  void Reset();

private:
  std::unordered_map<std::uint32_t, Clock::time_point> lastAttack;
};

} // namespace SkyrimRemember
