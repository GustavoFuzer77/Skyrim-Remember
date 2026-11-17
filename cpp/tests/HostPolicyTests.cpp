// Host-side policy that needs no game: outfit capture, profiles, guard reactions
// and the notices they produce.

#include "TestSupport.h"
#include "GuardNotices.h"
#include "GuardReactionAdapter.h"
#include "NpcProfile.h"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <iterator>

using namespace SkyrimRemember;
using namespace SkyrimRemember::Testing;

TEST_SUITE_BEGIN("host policy");

TEST_CASE("OutfitCapture") {
  const std::array pieces = {
      WornPiece{0x2000, handsSlot},
      WornPiece{0, bodySlot},      // no Form: ignored entirely
      WornPiece{0x1000, bodySlot}, // one Form worn in two slots
      WornPiece{0x1000, 1U << 16}, //   ... merges into one item
      WornPiece{0x3000, 0},        // no slots: ignored entirely
  };

  const auto merged = MergeWornPieces(pieces);

  REQUIRE(merged.items.size() == 2);
  REQUIRE(merged.items[0].formId == 0x1000);
  REQUIRE(merged.items[0].slotMask == (bodySlot | (1U << 16)));
  REQUIRE(merged.items[1].formId == 0x2000);
  REQUIRE(merged.faceVisible);

  // A helmet taking the head slot hides the face; a circlet alone does not.
  const std::array helmet = {WornPiece{0x4000, Biped::Head}};
  const std::array hood = {WornPiece{0x5000, Biped::Hair | Biped::Circlet}};
  const std::array circlet = {WornPiece{0x6000, Biped::Circlet}};
  const std::array unreadable = {WornPiece{0, Biped::Head}};

  REQUIRE(!MergeWornPieces(helmet).faceVisible);
  REQUIRE(!MergeWornPieces(hood).faceVisible);
  REQUIRE(MergeWornPieces(circlet).faceVisible);
  REQUIRE(MergeWornPieces(unreadable).faceVisible);
  REQUIRE(MergeWornPieces({}).faceVisible);
  REQUIRE(MergeWornPieces({}).items.empty());

  // Technical decapitation slots never reach the visual signature.
  const std::array technicalOnly = {WornPiece{0x7000, technicalSlots}};
  REQUIRE(MergeWornPieces(technicalOnly).items.empty());

  // A piece mixing visual and technical slots keeps only the visual bits.
  const std::array mixed = {WornPiece{0x8000, bodySlot | Biped::DecapitateHead}};
  const auto mergedMixed = MergeWornPieces(mixed);
  REQUIRE(mergedMixed.items.size() == 1);
  REQUIRE(mergedMixed.items[0].slotMask == bodySlot);

  // A helmet that also carries kDecapitateHead still hides the face.
  const std::array technicalHelmet = {WornPiece{0x9000, Biped::Head | Biped::Decapitate}};
  const auto mergedHelmet = MergeWornPieces(technicalHelmet);
  REQUIRE(mergedHelmet.items.size() == 1);
  REQUIRE(mergedHelmet.items[0].slotMask == Biped::Head);
  REQUIRE(!mergedHelmet.faceVisible);

  // Bits 10 and 11 are tail and long hair, not technical slots.
  constexpr std::uint32_t tailAndLongHair = (1U << 10) | (1U << 11);
  const std::array tail = {WornPiece{0xA000, tailAndLongHair}};
  const auto mergedTail = MergeWornPieces(tail);
  REQUIRE(mergedTail.items.size() == 1);
  REQUIRE(mergedTail.items[0].slotMask == tailAndLongHair);
}

TEST_CASE("ProfilePolicy") {
  // Appearance memory is universal even before every profile has reactions.
  REQUIRE(IsProfileEnabled(NpcProfile::Guard));
  REQUIRE(IsProfileEnabled(NpcProfile::Follower));
  REQUIRE(IsProfileEnabled(NpcProfile::Citizen));
  REQUIRE(IsProfileEnabled(NpcProfile::Merchant));
  REQUIRE(IsProfileEnabled(NpcProfile::Generic));

  // Every profile is nameable and every enum value stays inside the range the
  // metrics array is sized by.
  constexpr NpcProfile every[] = {NpcProfile::Guard, NpcProfile::Follower, NpcProfile::Citizen, NpcProfile::Merchant, NpcProfile::Generic};
  REQUIRE(std::size(every) == npcProfileCount);
  for (std::size_t index = 0; index < std::size(every); ++index) {
    REQUIRE(static_cast<std::size_t>(every[index]) == index);
    REQUIRE(ProfileName(every[index]) != nullptr);
    REQUIRE(std::strlen(ProfileName(every[index])) > 0);
  }
  REQUIRE(std::strcmp(ProfileName(NpcProfile::Guard), "Guard") == 0);
  REQUIRE(std::strcmp(ProfileName(NpcProfile::Merchant), "Merchant") == 0);
}

TEST_CASE("GuardReactionPolicy") {
  // The first recognition of a session or an encounter is a reaction, a
  // stranger's first sighting is not.
  IdentityObservation identity{};
  identity.recognitionChanged = true;
  identity.appearance.recognized = true;
  REQUIRE(DecideGuardReaction(identity) == GuardReaction::Recognized);

  identity.newObserver = true;
  REQUIRE(DecideGuardReaction(identity) == GuardReaction::None);

  identity.newObserver = false;
  identity.appearance.recognized = false;
  REQUIRE(DecideGuardReaction(identity) == GuardReaction::Unrecognized);

  identity.recognitionChanged = false;
  REQUIRE(DecideGuardReaction(identity) == GuardReaction::None);
}

TEST_CASE("GuardNotices") {
  // The notice board groups, spaces and cancels notices without ever touching a
  // guard's memory or threshold.
  namespace Notice = SkyrimRemember::GuardNoticeTuning;
  using std::chrono::seconds;

  const auto start = GuardNoticeBoard::Clock::time_point{} + std::chrono::hours{1};

  // The very first recognition is shown, and guards recognizing together are
  // grouped into one notice.
  GuardNoticeBoard board;
  board.Record(1, GuardReaction::Recognized);
  board.Record(2, GuardReaction::Recognized);
  board.Record(3, GuardReaction::Unrecognized); // never announced: not news
  auto summary = board.Flush(start);
  REQUIRE((summary.recognized == 2 && summary.unrecognized == 0));
  REQUIRE(board.Tracked() == 2);

  // Inside the grouping interval nothing is shown, but nothing is lost.
  board.Record(4, GuardReaction::Recognized);
  REQUIRE(board.Flush(start + seconds{1}).Empty());
  summary = board.Flush(start + Notice::groupingInterval);
  REQUIRE(summary.recognized == 1);

  // Flicker around the threshold cancels out before it reaches the HUD.
  auto now = start + Notice::groupingInterval * 2;
  board.Record(1, GuardReaction::Unrecognized);
  board.Record(1, GuardReaction::Recognized);
  REQUIRE(board.Flush(now).Empty());

  // A real loss waits out the per-guard spacing, then is shown once.
  board.Record(2, GuardReaction::Unrecognized);
  REQUIRE(board.Flush(now).Empty());
  now = start + Notice::perGuardSpacing;
  summary = board.Flush(now);
  REQUIRE((summary.recognized == 0 && summary.unrecognized == 1));

  // Recognizing again in the same encounter window is not repeated...
  board.Record(1, GuardReaction::Recognized);
  REQUIRE(board.Flush(now + Notice::groupingInterval).Empty());

  // ...but a later encounter is announced again, and stale guards are pruned.
  now = start + Notice::repeatAfter + Notice::perGuardSpacing;
  board.Record(1, GuardReaction::Recognized);
  summary = board.Flush(now);
  REQUIRE(summary.recognized == 1);
  REQUIRE(board.Tracked() == 1);

  board.Reset();
  REQUIRE(board.Tracked() == 0);

  REQUIRE(FormatGuardNotice(GuardReaction::Recognized, 1) == "A guard recognizes you.");
  REQUIRE(FormatGuardNotice(GuardReaction::Recognized, 3) == "3 guards recognize you.");
  REQUIRE(FormatGuardNotice(GuardReaction::Unrecognized, 1) == "A guard no longer recognizes you.");
  REQUIRE(FormatGuardNotice(GuardReaction::Unrecognized, 2) == "2 guards no longer recognize you.");
  REQUIRE(FormatGuardNotice(GuardReaction::Recognized, 0).empty());
}

TEST_SUITE_END();
