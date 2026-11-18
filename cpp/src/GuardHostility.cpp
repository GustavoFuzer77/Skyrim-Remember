#include "GuardHostility.h"

namespace SkyrimRemember {

const char *HostilityVerdictName(HostilityVerdict verdict) {
  switch (verdict) {
  case HostilityVerdict::Calm:
    return "calm";
  case HostilityVerdict::Attack:
    return "attack";
  case HostilityVerdict::Disabled:
    return "disabled";
  case HostilityVerdict::NotRecognized:
    return "not recognized";
  case HostilityVerdict::InCombat:
    return "in combat";
  case HostilityVerdict::CoolingDown:
    return "cooling down";
  }
  return "unknown";
}

HostilityVerdict GuardHostility::Decide(std::uint32_t guard, bool recognized, float suspicion, bool inCombat, Clock::time_point now,
                                        const HostilitySettings &settings) {
  if (!settings.enabled) {
    return HostilityVerdict::Disabled;
  }
  if (!recognized) {
    return HostilityVerdict::NotRecognized;
  }
  if (suspicion < settings.suspicionThreshold) {
    return HostilityVerdict::Calm;
  }
  if (inCombat) {
    return HostilityVerdict::InCombat;
  }
  if (const auto found = lastAttack.find(guard); found != lastAttack.end() && now - found->second < settings.cooldown) {
    return HostilityVerdict::CoolingDown;
  }
  lastAttack[guard] = now;
  return HostilityVerdict::Attack;
}

void GuardHostility::Reset() {
  lastAttack.clear();
}

} // namespace SkyrimRemember
