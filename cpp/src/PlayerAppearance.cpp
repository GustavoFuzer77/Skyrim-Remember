#include "PlayerAppearance.h"

#include <RE/B/BGSBipedObjectForm.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESObjectARMO.h>

#include <cstdint>
#include <vector>

namespace SkyrimRemember {
namespace {
// Appearance.h keeps these bits so it can stay game-free; this is where the
// two definitions are held to each other.
constexpr auto SlotBit(RE::BGSBipedObjectForm::BipedObjectSlot slot) {
  return static_cast<std::uint32_t>(slot);
}

static_assert(BipedSlot::Head == SlotBit(RE::BGSBipedObjectForm::BipedObjectSlot::kHead));
static_assert(BipedSlot::Hair == SlotBit(RE::BGSBipedObjectForm::BipedObjectSlot::kHair));
static_assert(BipedSlot::Circlet == SlotBit(RE::BGSBipedObjectForm::BipedObjectSlot::kCirclet));
static_assert(BipedSlot::DecapitateHead == SlotBit(RE::BGSBipedObjectForm::BipedObjectSlot::kDecapitateHead));
static_assert(BipedSlot::Decapitate == SlotBit(RE::BGSBipedObjectForm::BipedObjectSlot::kDecapitate));

// Bits 10 and 11 are real appearance, not technical: guarding against a future
// edit that lumps them in with the decapitation slots.
static_assert((technicalSlots & SlotBit(RE::BGSBipedObjectForm::BipedObjectSlot::kTail)) == 0);
static_assert((technicalSlots & SlotBit(RE::BGSBipedObjectForm::BipedObjectSlot::kLongHair)) == 0);

constexpr std::uint32_t bipedSlotCount = 32;
} // namespace

CapturedAppearance CapturePlayerAppearance() {
  auto *player = RE::PlayerCharacter::GetSingleton();
  if (!player) {
    return {};
  }

  std::vector<WornPiece> worn;
  worn.reserve(bipedSlotCount);
  for (std::uint32_t bit = 0; bit < bipedSlotCount; ++bit) {
    const auto slotMask = std::uint32_t{1} << bit;
    const auto slot = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(slotMask);
    auto *armor = player->GetWornArmor(slot);
    if (!armor) {
      continue;
    }
    worn.push_back({armor->GetFormID(), static_cast<std::uint32_t>(armor->GetSlotMask())});
  }
  return MergeWornPieces(worn);
}

} // namespace SkyrimRemember
