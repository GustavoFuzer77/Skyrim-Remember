#include "Defines.h"
#include "NpcProfile.h"

#include <RE/A/Actor.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESFaction.h>
#include <SKSE/SKSE.h>

#include <string_view>

namespace SkyrimRemember {
namespace {

RE::TESFaction *whiterunGuardFaction = nullptr;
// add new factions here (types)

// The keyword every humanoid NPC record carries; creatures do not.
constexpr std::string_view npcTypeKeyword = "ActorTypeNPC";

} // namespace

bool InitializeClassification() {

  auto *data = RE::TESDataHandler::GetSingleton();

  whiterunGuardFaction = data ? data->LookupForm<RE::TESFaction>(FormIds::WhiterunGuardFactionLocalID, "Skyrim.esm") : nullptr;

  if (!whiterunGuardFaction) {
    SKSE::log::error("Classification disabled: GuardFactionWhiterun not found");
    return false;
  }

  SKSE::log::info("Classification ready: GuardFactionWhiterun={:08X}", whiterunGuardFaction->GetFormID());
  return true;
}

NpcProfile ClassifyNpc(RE::Actor &actor) {

  auto *player = RE::PlayerCharacter::GetSingleton();

  if (&actor == player) {
    return NpcProfile::Generic;
  }

  // Guard is the only profile with behaviour today, and it is deliberately not
  // geographic: faction membership decides, so a Whiterun guard met on the road
  // is still the same guard. Civil War and mods can rewrite factions and
  // templates, which is why the test stays this conservative — an actor that
  // stops matching simply stops being observed, and the memory it already has
  // is left untouched rather than reassigned.
  if (whiterunGuardFaction && actor.IsGuard() && actor.IsInFaction(whiterunGuardFaction)) {
    return NpcProfile::Guard;
  }

  if (actor.IsPlayerTeammate()) {
    return NpcProfile::Follower;
  }

  // Hostility is a state, not a role. An actor currently fighting the Player is
  // parked in Generic rather than being called a Citizen.
  if (player && actor.IsHostileToActor(player)) {
    return NpcProfile::Generic;
  }

  // TODO(merchant): a vendor-faction test belongs here, ahead of the Citizen
  // fallback. It needs a faction walk too expensive for the scan as written, so
  // Merchant is currently a category with no classifier.

  if (actor.HasKeywordString(npcTypeKeyword)) {
    return NpcProfile::Citizen;
  }

  return NpcProfile::Generic;
}

} // namespace SkyrimRemember
