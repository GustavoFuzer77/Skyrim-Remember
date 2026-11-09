#pragma once

#include "Memory.h"

#include <cstdint>
#include <span>
#include <vector>

// Everything in this header is free of Skyrim dependencies on purpose: it is
// the half of the appearance capture that can be exercised by MemoryTests
// without the game installed. PlayerAppearance.cpp owns the half that reads
// the live PlayerCharacter and static_asserts these constants against
// CommonLibSSE's own enum.
namespace SkyrimRemember {

// Mirrors RE::BGSBipedObjectForm::BipedObjectSlot. These are bit indices, not
// Creation Kit slot numbers: bit 20 is CK slot 50. PlayerAppearance.cpp holds
// every constant here against the CommonLibSSE enum with static_asserts.
namespace BipedSlot {

inline constexpr std::uint32_t Head = 1U << 0;            // kHead
inline constexpr std::uint32_t Hair = 1U << 1;            // kHair
inline constexpr std::uint32_t Circlet = 1U << 12;        // kCirclet
inline constexpr std::uint32_t DecapitateHead = 1U << 20; // kDecapitateHead
inline constexpr std::uint32_t Decapitate = 1U << 21;     // kDecapitate

} // namespace BipedSlot

// Bits that carry no visual meaning: the game uses them for decapitation
// geometry, never for something an onlooker could describe. Note that bits 10
// and 11 (kTail, kLongHair) are NOT technical and do take part in appearance.
inline constexpr std::uint32_t technicalSlots = BipedSlot::DecapitateHead | BipedSlot::Decapitate;

static_assert(technicalSlots == 0x00300000U);

// A piece hides the face when it takes the head slot outright, or when it
// occupies hair and circlet together the way full helmets do.
constexpr bool CoversFace(std::uint32_t slotMask) {
  return (slotMask & BipedSlot::Head) != 0 || ((slotMask & BipedSlot::Hair) != 0 && (slotMask & BipedSlot::Circlet) != 0);
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

// The Player's outfit in the shape the memory store expects.
struct CapturedAppearance {
  std::vector<AppearanceItem> items;
  // Keyword FormIDs carried by the worn items, sorted and unique. Impression
  // rules match these; recognition ignores them.
  std::vector<std::uint32_t> keywords;
  bool faceVisible = true;

  bool operator==(const CapturedAppearance &) const = default;
};

// Strips the technical slots from every piece, drops pieces the game could not
// describe or that had nothing but technical bits, merges the slots of one Form
// worn in several slots, sorts by FormID so equal outfits compare equal, and
// reports whether anything hides the face.
//
// A piece that mixes visual and technical slots keeps its visual bits; only the
// technical ones are cleared.
CapturedAppearance MergeWornPieces(std::span<const WornPiece> pieces);

} // namespace SkyrimRemember
