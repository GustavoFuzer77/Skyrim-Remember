#pragma once

#include "Appearance.h"

namespace SkyrimRemember {

// Reads every armor slot of the live PlayerCharacter. Returns an empty, fully
// visible appearance when the Player singleton is unavailable.
CapturedAppearance CapturePlayerAppearance();

} // namespace SkyrimRemember
