#pragma once

#include "RustBridge.h"

#include <cstdint>
#include <span>
#include <vector>

// Everything in this header is free of Skyrim dependencies on purpose: it is
// the half of the appearance capture that can be exercised by BridgeSmoke
// without the game installed. PlayerAppearance.cpp owns the half that reads
// the live PlayerCharacter and static_asserts these constants against
// CommonLibSSE's own enum.
namespace SkyrimRemember {

// Mirrors RE::BGSBipedObjectForm::BipedObjectSlot.
namespace BipedSlot {
inline constexpr std::uint32_t Head = 1U << 0;
inline constexpr std::uint32_t Hair = 1U << 1;
inline constexpr std::uint32_t Circlet = 1U << 12;
} // namespace BipedSlot

// A piece hides the face when it takes the head slot outright, or when it
// occupies hair and circlet together the way full helmets do.
constexpr bool CoversFace(std::uint32_t slotMask) {
  return (slotMask & BipedSlot::Head) != 0 ||
         ((slotMask & BipedSlot::Hair) != 0 &&
          (slotMask & BipedSlot::Circlet) != 0);
}

static_assert(CoversFace(BipedSlot::Head));
static_assert(CoversFace(BipedSlot::Hair | BipedSlot::Circlet));
static_assert(!CoversFace(BipedSlot::Circlet));
static_assert(!CoversFace(BipedSlot::Hair));

// One worn armor Form exactly as the game reports it, before merging.
struct WornPiece {
  std::uint32_t formId = 0;
  std::uint32_t slotMask = 0;
};

// The Player's outfit in the shape the Rust core expects.
struct CapturedAppearance {
  std::vector<AppearanceItem> items;
  bool faceVisible = true;

  bool operator==(const CapturedAppearance &) const = default;
};

// Drops pieces the game could not describe, merges the slots of one Form worn
// in several slots, sorts by FormID so equal outfits compare equal, and reports
// whether anything hides the face.
CapturedAppearance MergeWornPieces(std::span<const WornPiece> pieces);

} // namespace SkyrimRemember
