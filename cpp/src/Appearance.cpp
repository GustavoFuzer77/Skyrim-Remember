#include "Appearance.h"

#include <algorithm>

namespace SkyrimRemember {

CapturedAppearance MergeWornPieces(std::span<const WornPiece> pieces) {
  CapturedAppearance captured;
  captured.items.reserve(pieces.size());
  for (const auto &piece : pieces) {
    // A Form the game cannot describe says nothing about the face either.
    if (piece.formId == 0 || piece.slotMask == 0) {
      continue;
    }
    captured.faceVisible = captured.faceVisible && !CoversFace(piece.slotMask);
    const auto existing =
        std::ranges::find_if(captured.items, [&piece](const AppearanceItem &item) {
          return item.formId == piece.formId;
        });
    if (existing != captured.items.end()) {
      existing->slotMask |= piece.slotMask;
    } else {
      captured.items.push_back({piece.formId, piece.slotMask});
    }
  }
  std::ranges::sort(captured.items, {}, &AppearanceItem::formId);
  return captured;
}

} // namespace SkyrimRemember
