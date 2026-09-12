#include "Appearance.h"

#include <algorithm>

namespace SkyrimRemember {

CapturedAppearance MergeWornPieces(std::span<const WornPiece> pieces) {
  CapturedAppearance captured;
  captured.items.reserve(pieces.size());
  for (const auto &piece : pieces) {
    // Keep the visual bits, discard the technical ones. A piece left with
    // nothing visual is not part of the signature at all — and a Form the game
    // cannot describe says nothing about the face either.
    const auto visualMask = piece.slotMask & ~technicalSlots;
    if (piece.formId == 0 || visualMask == 0) {
      continue;
    }
    captured.faceVisible = captured.faceVisible && !CoversFace(visualMask);
    const auto existing = std::ranges::find_if(captured.items, [&piece](const AppearanceItem &item) {
      return item.formId == piece.formId;
    });
    if (existing != captured.items.end()) {
      existing->slotMask |= visualMask;
    } else {
      captured.items.push_back({piece.formId, visualMask});
    }
  }
  std::ranges::sort(captured.items, {}, &AppearanceItem::formId);
  return captured;
}

} // namespace SkyrimRemember
