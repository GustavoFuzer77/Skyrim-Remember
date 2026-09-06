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
    return true;
  // Classified, counted in the diagnostics, and otherwise inert: these profiles
  // must not create memory, call the Rust core, capture appearance, or consume
  // the observation budget until they have behaviour of their own. Flipping one
  // of these to true is all it should take to bring it into the scan.
  case NpcProfile::Follower:
  case NpcProfile::Citizen:
  case NpcProfile::Merchant:
  case NpcProfile::Generic:
    return false;
  }
  return false;
}

} // namespace SkyrimRemember
