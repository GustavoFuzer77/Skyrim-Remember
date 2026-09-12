#include "NpcProfile.h"

// Deliberately free of Skyrim dependencies: the profile policy is the part of
// classification that can be exercised by BridgeSmoke without the game.
// NpcClassification.cpp holds the half that reads a live Actor.

namespace SkyrimRemember {

const char *ProfileName(NpcProfile profile) {
  switch (profile) {
  case NpcProfile::Guard:
    return "Guard";
  case NpcProfile::Follower:
    return "Follower";
  case NpcProfile::Citizen:
    return "Citizen";
  case NpcProfile::Merchant:
    return "Merchant";
  case NpcProfile::Generic:
    return "Generic";
  }
  return "Generic";
}

bool IsProfileEnabled(NpcProfile profile) {
  switch (profile) {
  case NpcProfile::Guard:
  case NpcProfile::Follower:
  case NpcProfile::Citizen:
  case NpcProfile::Merchant:
  case NpcProfile::Generic:
    // Appearance memory belongs to every NPC. Profiles only decide which
    // reaction adapter consumes the universal recognition result afterwards.
    return true;
  }
  return false;
}

} // namespace SkyrimRemember
