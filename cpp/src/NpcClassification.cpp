#include "NpcProfile.h"

#include <RE/A/Actor.h>
#include <RE/P/PlayerCharacter.h>

#include <string_view>

namespace SkyrimRemember {
namespace {

// The keyword every humanoid NPC record carries; creatures do not.
constexpr std::string_view npcTypeKeyword = "ActorTypeNPC";

} // namespace

std::optional<NpcProfile> ClassifyNpc(RE::Actor &actor) {

  auto *player = RE::PlayerCharacter::GetSingleton();

  if (&actor == player) {
    return std::nullopt;
  }

  // ProcessLists also contains creatures. Appearance memory belongs to every
  // humanoid NPC profile, not to animals, dragons, or the Player.
  if (!actor.HasKeywordString(npcTypeKeyword)) {
    return std::nullopt;
  }

  // The engine itself flags guards, the same flag its crime and dialogue
  // systems read. Nothing here names a hold or a faction, so the guards of every
  // city, guards swapped by the Civil War, and guards added by mods are all one
  // profile, with no list to keep up to date. The hold a guard serves is
  // actor.GetCrimeFaction(), should a reaction ever need it.
  if (actor.IsGuard()) {
    return NpcProfile::Guard;
  }

  if (actor.IsPlayerTeammate()) {
    return NpcProfile::Follower;
  }

  if (actor.GetVendorFaction()) {
    return NpcProfile::Merchant;
  }

  // Hostility is a state, not a role. An actor currently fighting the Player is
  // parked in Generic rather than being called a Citizen.
  if (player && actor.IsHostileToActor(player)) {
    return NpcProfile::Generic;
  }

  return NpcProfile::Citizen;
}

} // namespace SkyrimRemember
