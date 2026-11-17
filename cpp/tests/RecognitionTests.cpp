// Appearance and recognition: how an NPC decides it has seen the Player before.

#include "TestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <vector>

using namespace SkyrimRemember;
using namespace SkyrimRemember::Testing;

TEST_SUITE_BEGIN("recognition");

TEST_CASE("SimilarityIsWeightedBySlotGroup") {
  const AppearanceWeights weights;
  REQUIRE(OutfitSimilarity({}, {}, weights) == 1.0F); // two empty outfits are identical

  const std::array left = {Item(10, bodySlot), Item(20, handsSlot)};
  const std::array right = {Item(10, bodySlot), Item(30, handsSlot)};
  // Matching body weight 0.35 over applicable body+hands weight 0.45.
  REQUIRE(Near(OutfitSimilarity(left, right, weights), 0.35F / 0.45F));

  AppearanceWeights heavyHands;
  heavyHands.hands = 0.35F;
  REQUIRE(Near(OutfitSimilarity(left, right, heavyHands), 0.5F));
}

TEST_CASE("NormalizingMergesAndStrips") {
  std::vector<AppearanceItem> items = {Item(20, 1U << 3), Item(0, 1U << 2), Item(10, 1U << 2), Item(10, 1U << 16), Item(30, 0)};
  NormalizeItems(items);
  REQUIRE((items == std::vector{Item(10, (1U << 2) | (1U << 16)), Item(20, 1U << 3)}));

  // Technical bits leave the signature: a piece with only those disappears, a
  // mixed piece keeps its visual bits, and tail/long hair are not technical.
  std::vector<AppearanceItem> technical = {Item(10, technicalSlots), Item(20, (1U << 2) | (1U << 20)), Item(30, 1U << 10)};
  NormalizeItems(technical);
  REQUIRE((technical == std::vector{Item(20, 1U << 2), Item(30, 1U << 10)}));

  std::vector<AppearanceItem> tail = {Item(10, (1U << 10) | (1U << 11))};
  NormalizeItems(tail);
  REQUIRE((tail == std::vector{Item(10, (1U << 10) | (1U << 11))}));
}

TEST_CASE("CoveredFaceNeedsAMatchingOutfit") {
  MemoryStore store;
  Know(store, 123);

  const auto first = Observe(store, 123, {Item(10)}, false, false, 1.0);
  REQUIRE((!first.appearanceMatched && !first.recognized && first.learned));

  const auto same = Observe(store, 123, {Item(10)}, false, false, 1.1);
  REQUIRE((same.appearanceMatched && !same.recognized && !same.learned));
  REQUIRE(same.outfitSimilarity == 1.0F);

  const auto disguise = Observe(store, 123, {Item(20)}, false, false, 1.2);
  REQUIRE(!disguise.recognized);
  REQUIRE(disguise.identityConfidence == 0.0F);
  REQUIRE(disguise.knownAppearances == 1);
}

TEST_CASE("AVisibleFaceConnectsANewOutfit") {
  MemoryStore store;
  Know(store, 123);
  Observe(store, 123, {Item(10)}, true, false, 1.0);

  const auto changed = Observe(store, 123, {Item(20)}, true, false, 1.1);
  REQUIRE((changed.recognized && changed.learned));
  REQUIRE((changed.outfitSimilarity == 0.0F && changed.identityConfidence == 1.0F));
  REQUIRE(changed.knownAppearances == 2);
  REQUIRE(changed.faceVisible);
}

TEST_CASE("RevealingTheFaceLinksAnAnonymousOutfit") {
  MemoryStore store;
  Know(store, 123);

  REQUIRE(!Observe(store, 123, {Item(10)}, false, false, 1.0).recognized);

  const auto revealed = Observe(store, 123, {Item(10)}, true, false, 1.1);
  REQUIRE((revealed.recognized && revealed.appearanceMatched));

  const auto hiddenAgain = Observe(store, 123, {Item(10)}, false, false, 1.2);
  REQUIRE((hiddenAgain.recognized && hiddenAgain.appearanceMatched));
}

TEST_CASE("VisualContinuity") {
  {
    // Uninterrupted contact links a completely different hidden outfit.
    MemoryStore store;
    Know(store, 123);
    Observe(store, 123, {Item(10)}, true, false, 1.0);

    const auto changed = Observe(store, 123, {Item(20)}, false, true, 1.1);
    REQUIRE((changed.recognized && changed.linkedByContinuity && changed.learned));
    REQUIRE(changed.outfitSimilarity == 0.0F);

    const auto seenLater = Observe(store, 123, {Item(20)}, false, false, 1.2);
    REQUIRE((seenLater.recognized && !seenLater.linkedByContinuity));
  }
  {
    // Once contact is lost, a hidden change is a stranger.
    MemoryStore store;
    Know(store, 123);
    Observe(store, 123, {Item(10)}, true, false, 1.0);

    const auto changed = Observe(store, 123, {Item(20)}, false, false, 1.1);
    REQUIRE((!changed.recognized && !changed.linkedByContinuity && !changed.learned));
  }
  {
    // Host-verified contact can establish identity on its own.
    MemoryStore store;
    Know(store, 123);
    const auto hidden = Observe(store, 123, {Item(10)}, false, true, 1.0);
    REQUIRE((hidden.recognized && hidden.linkedByContinuity));
  }
}

TEST_CASE("EvaluatingHasNoSideEffects") {
  MemoryStore store;
  Know(store, 123);

  const std::array outfit = {Item(10)};
  AppearanceResult evaluation;
  REQUIRE(store.EvaluateAppearance(123, outfit, true, false, evaluation) == MemoryStatus::Ok);
  REQUIRE((evaluation.recognized && !evaluation.learned && evaluation.knownAppearances == 0));
  REQUIRE(store.KnownAppearances(123).empty());

  // Evaluation reports the normalized item count, not the raw one.
  const std::array raw = {Item(10, bodySlot), Item(10, handsSlot), Item(0, bodySlot), Item(20, technicalSlots)};
  REQUIRE(store.EvaluateAppearance(123, raw, true, false, evaluation) == MemoryStatus::Ok);
  REQUIRE(evaluation.itemCount == 1);

  // It does not create the memory, either.
  REQUIRE(store.EvaluateAppearance(999, outfit, true, false, evaluation) == MemoryStatus::NotFound);
  REQUIRE(store.Size() == 1);
  REQUIRE(store.EvaluateAppearance(0, outfit, true, false, evaluation) == MemoryStatus::InvalidId);
}

TEST_CASE("ThresholdChangesWithFamiliarity") {
  {
    // The thin line the initial seventy percent draws: only the helmet differs.
    MemoryStore store;
    const std::vector<AppearanceItem> first = {Item(1, 1U << 7), Item(2, 1U << 3), Item(3, 1U << 2), Item(4, (1U << 1) | (1U << 12)), Item(5, 1U << 22), Item(6, 1U << 6)};
    auto helmetChanged = first;
    helmetChanged[3].formId = 7;

    // An NPC that already knows this outfit as the Player's, restored from a save.
    Payload payload;
    payload.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(123, {}, 1.0);
    payload.Put<std::uint64_t>(1).Put<std::uint64_t>(first.size());
    for (const auto &item : first) {
      payload.Put(item.formId).Put(item.slotMask);
    }
    payload.Put<std::uint8_t>(0).Put<std::uint8_t>(1).Put(1.0);
    PersistenceStats stats;
    REQUIRE(store.Deserialize(payload.bytes, keepIds, stats) == MemoryStatus::Ok);

    const auto result = Observe(store, 123, helmetChanged, false, false, 1.1);
    REQUIRE(Near(result.outfitSimilarity, 0.7058823F));
    REQUIRE(Near(result.recognitionThreshold, 0.70F));
    REQUIRE((result.recognized && result.learned));
  }
  {
    // Full familiarity lowers the bar to the floor.
    MemoryStore store;
    MemoryDelta delta{};
    Slot(delta, Axis::Recognition) = 100.0F;
    MemorySnapshot snapshot;
    store.ApplyDelta(123, 1.0, delta, snapshot);
    const auto first = Observe(store, 123, {Item(10)}, false, false, 1.0);
    REQUIRE(Near(first.recognitionThreshold, 0.50F));
  }
}

TEST_CASE("FamiliarityIsEarnedNotFarmed") {
  MemoryStore store;
  Know(store, 123);

  const auto first = Observe(store, 123, {Item(10)}, true, false, 1.0);
  REQUIRE(Near(first.recognitionThreshold, 0.70F));
  REQUIRE(Raw(store, 123, Axis::Recognition) == 5.0F);

  // Polling a moment later gains nothing.
  const auto polled = Observe(store, 123, {Item(10)}, true, false, 1.1);
  REQUIRE(Near(polled.recognitionThreshold, 0.69F));
  REQUIRE(Raw(store, 123, Axis::Recognition) == 5.0F);

  // A properly spaced encounter does.
  Observe(store, 123, {Item(10)}, true, false, 1.25);
  REQUIRE(Raw(store, 123, Axis::Recognition) == 10.0F);

  // A covered face never builds facial familiarity, continuity or not.
  MemoryStore covered;
  Know(covered, 123);
  Observe(covered, 123, {Item(10)}, false, false, 1.0);
  Observe(covered, 123, {Item(10)}, false, true, 2.0);
  REQUIRE(Raw(covered, 123, Axis::Recognition) == 0.0F);
}

TEST_CASE("OutfitCapacityAndRefresh") {
  {
    // The oldest outfit is evicted at the configured capacity.
    Tuning tuning;
    tuning.limits.maxKnownAppearances = 2;
    MemoryStore store(tuning);
    Know(store, 123);
    Observe(store, 123, {Item(10)}, true, false, 1.0);
    Observe(store, 123, {Item(20)}, true, false, 2.0);
    Observe(store, 123, {Item(30)}, true, false, 3.0);

    const auto known = store.KnownAppearances(123);
    REQUIRE(known.size() == 2);
    REQUIRE(std::ranges::none_of(known, [](const KnownAppearance &entry) { return entry.items[0].formId == 10; }));
  }
  {
    // Polling inside one game hour does not keep an outfit fresh...
    MemoryStore store;
    Know(store, 123);
    Observe(store, 123, {Item(10)}, true, false, 1.0);
    for (int tick = 1; tick <= 20; ++tick) {
      REQUIRE(!Observe(store, 123, {Item(10)}, true, false, 1.0 + tick * 0.0005).learned);
    }
    REQUIRE(store.KnownAppearances(123).size() == 1);
    REQUIRE(store.KnownAppearances(123)[0].lastSeenGameDays == 1.0);

    // ...but it is refreshed once the interval has really passed.
    Observe(store, 123, {Item(10)}, true, false, 1.5);
    REQUIRE(store.KnownAppearances(123)[0].lastSeenGameDays == 1.5);
  }
  {
    // So polling never makes the newest outfit immune to eviction.
    Tuning tuning;
    tuning.limits.maxKnownAppearances = 2;
    MemoryStore store(tuning);
    Know(store, 123);
    Observe(store, 123, {Item(10)}, true, false, 1.0);
    Observe(store, 123, {Item(20)}, true, false, 2.0);
    for (int tick = 0; tick < 50; ++tick) {
      Observe(store, 123, {Item(10)}, true, false, 2.0 + tick * 0.0005);
    }
    Observe(store, 123, {Item(30)}, true, false, 3.0);

    const auto known = store.KnownAppearances(123);
    REQUIRE(known.size() == 2);
    REQUIRE(std::ranges::none_of(known, [](const KnownAppearance &entry) { return entry.items[0].formId == 10; }));
  }
}

TEST_CASE("ItemLimitsAndArguments") {
  Tuning tuning;
  tuning.limits.maxAppearanceItems = 2;
  MemoryStore store(tuning);
  Know(store, 123);
  const std::array tooMany = {Item(10, bodySlot), Item(20, handsSlot), Item(30, 1U << 7)};
  AppearanceResult result;
  REQUIRE(store.ObserveAppearance(123, 1.0, tooMany, true, false, result) == MemoryStatus::InvalidArgument);
  REQUIRE(store.EvaluateAppearance(123, tooMany, true, false, result) == MemoryStatus::InvalidArgument);
  REQUIRE(store.KnownAppearances(123).empty());

  // The hard cap applies whatever the tuning says.
  MemoryStore plain;
  Know(plain, 123);
  std::vector<AppearanceItem> huge;
  for (std::uint32_t index = 1; index <= maxAppearanceItems + 1; ++index) {
    huge.push_back(Item(index));
  }
  REQUIRE(plain.ObserveAppearance(123, 1.0, huge, true, false, result) == MemoryStatus::InvalidArgument);

  REQUIRE(plain.ObserveAppearance(999, 1.0, {}, true, false, result) == MemoryStatus::NotFound);
  REQUIRE(plain.ObserveAppearance(0, 1.0, {}, true, false, result) == MemoryStatus::InvalidId);
  REQUIRE(plain.ObserveAppearance(123, -1.0, {}, true, false, result) == MemoryStatus::InvalidArgument);
  REQUIRE(plain.Size() == 1);
}

TEST_CASE("StatusSequenceOfAWholeEncounter") {
  // The path a guard actually takes, end to end, through the public calls.
  MemoryStore store;
  Know(store, 123, 10.0);
  const std::array outfit = {Item(0x1000, 1U << 2), Item(0x2000, 1U << 3)};

  AppearanceResult result;
  REQUIRE(store.EvaluateAppearance(123, outfit, true, false, result) == MemoryStatus::Ok);
  REQUIRE((result.recognized && !result.learned && result.knownAppearances == 0));

  REQUIRE(store.ObserveAppearance(123, 10.0, outfit, false, false, result) == MemoryStatus::Ok);
  REQUIRE((!result.appearanceMatched && !result.recognized && result.learned && result.itemCount == 2 && result.knownAppearances == 1));

  REQUIRE(store.ObserveAppearance(123, 10.0, outfit, true, false, result) == MemoryStatus::Ok);
  REQUIRE((result.recognized && !result.linkedByContinuity));

  const std::array changed = {Item(0x3000, 1U << 2)};
  REQUIRE(store.ObserveAppearance(123, 10.0, changed, false, false, result) == MemoryStatus::Ok);
  REQUIRE((!result.recognized && !result.linkedByContinuity && !result.learned));

  REQUIRE(store.ObserveAppearance(123, 10.0, changed, false, true, result) == MemoryStatus::Ok);
  REQUIRE((result.recognized && result.linkedByContinuity && result.learned));
}

TEST_SUITE_END();
