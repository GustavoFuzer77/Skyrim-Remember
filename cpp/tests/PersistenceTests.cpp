// Persistence: the co-save payload, its migrations and its defenses.

#include "TestSupport.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <string_view>
#include <vector>

using namespace SkyrimRemember;
using namespace SkyrimRemember::Testing;

TEST_SUITE_BEGIN("persistence");

TEST_CASE("RoundTrip") {
  MemoryStore store;
  store.Register(0x456, 10.0);
  MemoryDelta fear{};
  Slot(fear, Axis::Fear) = 30.0F;
  MemorySnapshot snapshot;
  store.ApplyDelta(0x456, 10.0, fear, snapshot);
  store.Register(0x123, 11.0);
  MemoryDelta respect{};
  Slot(respect, Axis::Respect) = 50.0F;
  store.ApplyDelta(0x123, 11.0, respect, snapshot);
  Observe(store, 0x123, {Item(0x1000)}, true, false, 11.0);

  const auto first = store.Serialize();
  REQUIRE(first == store.Serialize()); // deterministic
  REQUIRE(!first.empty());

  store.Clear();
  PersistenceStats stats;
  REQUIRE(store.Deserialize(first, keepIds, stats) == MemoryStatus::Ok);
  REQUIRE((stats.loaded == 2 && stats.unresolved == 0));
  REQUIRE(store.Size() == 2);
  REQUIRE(Current(store, 0x123, Axis::Respect, 11.0) == 50.0F);
  REQUIRE(Current(store, 0x456, Axis::Fear, 10.0) == 30.0F);
  const auto memory = store.Find(0x123);
  REQUIRE((memory && memory->lastDecayGameDays == 11.0 && memory->lastRecognitionGameDays == 11.0));
  REQUIRE(store.KnownAppearances(0x123).at(0).items.at(0).formId == 0x1000);
  REQUIRE(store.Serialize() == first); // and stable across a reload

  // The reload kept the familiarity gained from the visible face.
  REQUIRE(store.Register(0x123, 11.0) == MemoryStatus::Ok);
  REQUIRE(Current(store, 0x123, Axis::Recognition, 11.0) == 5.0F);
}

// A payload the Rust core wrote (bincode 1.x): two NPCs, the second with an
// outfit seen with a visible face. Saves from before the core moved to C++ must
// keep loading.
TEST_CASE("ASavedFromTheRustCoreStillLoads") {
  constexpr std::string_view golden =
      "050000000200000000000000230100000000a0400000f040000000000000484200000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
      "000000000000000000000026400100000000000026400100000000000000020000000000000000100000040000000020000018000000010100000000000026405604000000000000000000"
      "000000f041000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002440000000000000000000";
  const auto bytes = FromHex(golden);
  REQUIRE(bytes.size() == 224);

  MemoryStore store;
  PersistenceStats stats;
  REQUIRE(store.Deserialize(bytes, keepIds, stats) == MemoryStatus::Ok);
  REQUIRE((stats.loaded == 2 && stats.unresolved == 0));

  MemorySnapshot snapshot;
  REQUIRE(store.Get(0x123, 11.0, snapshot) == MemoryStatus::Ok);
  REQUIRE((Level(snapshot, Axis::Recognition) == 5.0F && Level(snapshot, Axis::Suspicion) == 7.5F && Level(snapshot, Axis::Respect) == 50.0F));
  REQUIRE(store.Get(0x456, 10.0, snapshot) == MemoryStatus::Ok);
  REQUIRE(Level(snapshot, Axis::Fear) == 30.0F);
  REQUIRE(store.Find(0x123)->lastRecognitionGameDays == 11.0);
  REQUIRE(!store.Find(0x456)->lastRecognitionGameDays);

  const auto known = store.KnownAppearances(0x123);
  REQUIRE((known.size() == 1 && known[0].faceVisible && known[0].identityKnown && known[0].lastSeenGameDays == 11.0));
  REQUIRE((known[0].items == std::vector{Item(0x1000, 1U << 2), Item(0x2000, (1U << 3) | (1U << 4))}));

  // And writing it back reproduces the bytes exactly.
  REQUIRE(store.Serialize() == bytes);
}

TEST_CASE("FormsAreRemappedAndMissingOnesDropped") {
  MemoryStore store;
  store.Register(10, 1.0);
  MemoryDelta fear{};
  Slot(fear, Axis::Fear) = 20.0F;
  MemorySnapshot snapshot;
  store.ApplyDelta(10, 1.0, fear, snapshot);
  Observe(store, 10, {Item(20, bodySlot), Item(30, handsSlot)}, false, false, 1.0);
  const auto bytes = store.Serialize();

  PersistenceStats stats;
  const FormIdResolver remap = [](std::uint32_t old) -> std::optional<std::uint32_t> {
    switch (old) {
    case 10:
      return 110;
    case 20:
      return 120;
    default:
      return std::nullopt;
    }
  };
  REQUIRE(store.Deserialize(bytes, remap, stats) == MemoryStatus::Ok);
  REQUIRE((stats.loaded == 1 && stats.unresolved == 1));
  REQUIRE((store.Size() == 1 && !store.Find(10)));
  REQUIRE(Current(store, 110, Axis::Fear) == 20.0F);
  const auto known = store.KnownAppearances(110);
  REQUIRE((known.size() == 1 && known[0].items.size() == 1 && known[0].items[0].formId == 120));

  // An NPC that no longer resolves is skipped and counted, not an error.
  store.Clear();
  REQUIRE(store.Deserialize(bytes, [](std::uint32_t) { return std::nullopt; }, stats) == MemoryStatus::Ok);
  REQUIRE((stats.loaded == 0 && stats.unresolved == 1 && store.Size() == 0));
}

TEST_CASE("ASavedOutfitWithTechnicalBitsStillMatches") {
  // Restored the way an older save would have written it.
  Payload payload;
  payload.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(123, {}, 1.0);
  payload.Put<std::uint64_t>(1).Put<std::uint64_t>(1).Put<std::uint32_t>(10).Put<std::uint32_t>((1U << 2) | (1U << 20));
  payload.Put<std::uint8_t>(1).Put<std::uint8_t>(1).Put(1.0);

  MemoryStore store;
  PersistenceStats stats;
  REQUIRE(store.Deserialize(payload.bytes, keepIds, stats) == MemoryStatus::Ok);

  const auto seenAgain = Observe(store, 123, {Item(10, 1U << 2)}, true, false, 1.5);
  REQUIRE((seenAgain.outfitSimilarity == 1.0F && seenAgain.appearanceMatched));
  REQUIRE(!seenAgain.learned); // the same outfit must not be relearned
  REQUIRE(store.KnownAppearances(123).size() == 1);
}

TEST_CASE("CorruptedPayloadsAreRejectedAtomically") {
  MemoryStore store;
  store.Register(777, 1.0);
  PersistenceStats stats;

  REQUIRE(store.Deserialize(std::vector<std::uint8_t>{1, 2, 3}, keepIds, stats) == MemoryStatus::InvalidData);
  REQUIRE(store.Deserialize({}, keepIds, stats) == MemoryStatus::InvalidData);
  REQUIRE(store.Deserialize(std::vector<std::uint8_t>{1, 2, 3}, nullptr, stats) == MemoryStatus::InvalidArgument);

  Payload unsupported;
  unsupported.Put<std::uint32_t>(6).Put<std::uint64_t>(0);
  REQUIRE(store.Deserialize(unsupported.bytes, keepIds, stats) == MemoryStatus::UnsupportedVersion);
  Payload older;
  older.Put<std::uint32_t>(4).Put<std::uint64_t>(0);
  REQUIRE(store.Deserialize(older.bytes, keepIds, stats) == MemoryStatus::UnsupportedVersion);

  // A NaN axis is not something the game could have produced.
  std::array<float, axisCapacity> nan{};
  nan[AxisSlot(Axis::Fear)] = std::nanf("");
  Payload invalid;
  invalid.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(10, nan, 1.0).Put<std::uint64_t>(0);
  REQUIRE(store.Deserialize(invalid.bytes, keepIds, stats) == MemoryStatus::InvalidData);

  // A zero reference, an out-of-range axis, an absurd outfit count, a truncated
  // payload: all rejected.
  Payload zeroId;
  zeroId.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(0, {}, 1.0).Put<std::uint64_t>(0);
  REQUIRE(store.Deserialize(zeroId.bytes, keepIds, stats) == MemoryStatus::InvalidData);

  std::array<float, axisCapacity> huge{};
  huge[AxisSlot(Axis::Respect)] = 101.0F;
  Payload outOfRange;
  outOfRange.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(10, huge, 1.0).Put<std::uint64_t>(0);
  REQUIRE(store.Deserialize(outOfRange.bytes, keepIds, stats) == MemoryStatus::InvalidData);

  Payload absurd;
  absurd.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(10, {}, 1.0).Put<std::uint64_t>(0xFFFFFFFFFFFFFFFFULL);
  REQUIRE(store.Deserialize(absurd.bytes, keepIds, stats) == MemoryStatus::InvalidData);

  Payload tooManyOutfits;
  tooManyOutfits.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(10, {}, 1.0).Put<std::uint64_t>(maxKnownAppearances + 1);
  for (std::size_t index = 0; index <= maxKnownAppearances; ++index) {
    tooManyOutfits.Put<std::uint64_t>(0).Put<std::uint8_t>(1).Put<std::uint8_t>(1).Put(1.0);
  }
  REQUIRE(store.Deserialize(tooManyOutfits.bytes, keepIds, stats) == MemoryStatus::InvalidData);

  Payload badBool;
  badBool.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(10, {}, 1.0).Put<std::uint64_t>(1).Put<std::uint64_t>(0).Put<std::uint8_t>(7).Put<std::uint8_t>(0).Put(1.0);
  REQUIRE(store.Deserialize(badBool.bytes, keepIds, stats) == MemoryStatus::InvalidData);

  auto truncated = MemoryStore{};
  truncated.Register(5, 1.0);
  auto bytes = truncated.Serialize();
  bytes.pop_back();
  REQUIRE(store.Deserialize(bytes, keepIds, stats) == MemoryStatus::InvalidData);

  // None of that disturbed what the store already held.
  REQUIRE((store.Size() == 1 && store.Find(777)));
}

TEST_SUITE_END();
