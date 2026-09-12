#include "Appearance.h"
#include "NpcProfile.h"
#include "RustBridge.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

bool logged = false;

void Log(const char *message) noexcept {
  logged = message && std::strcmp(message, "Rust bridge initialized") == 0;
}

bool ResolveForm(void *, std::uint32_t oldFormID,
                 std::uint32_t *newFormID) noexcept {
  if (!newFormID) {
    return false;
  }
  *newFormID = oldFormID;
  return true;
}

} // namespace

// Check real C++ -> Rust calls in Release too (assert would be disabled).
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #condition);   \
      return 1;                                                                \
    }                                                                          \
  } while (false)

namespace {

// Reading and writing one axis slot, so the assertions say what they mean.
float &Slot(MemorySnapshot &snapshot, Axis axis) {
  return snapshot.axes[AxisSlot(axis)];
}

float &Slot(MemoryDelta &delta, Axis axis) {
  return delta.axes[AxisSlot(axis)];
}

namespace BipedSlot = SkyrimRemember::BipedSlot;

using SkyrimRemember::MergeWornPieces;
using SkyrimRemember::WornPiece;

constexpr std::uint32_t bodySlot = 1U << 2;
constexpr std::uint32_t handsSlot = 1U << 3;
} // namespace


// true entry poiny
int main() {

  CHECK(skyrim_remember_abi_version() == SkyrimRememberAbiVersion);

  // Appearance capture: the half that does not need Skyrim to run.
  {
    const std::array pieces = {
        WornPiece{0x2000, handsSlot},
        WornPiece{0, bodySlot},      // no Form: ignored entirely
        WornPiece{0x1000, bodySlot}, // one Form worn in two slots
        WornPiece{0x1000, 1U << 16}, //   ... merges into one item
        WornPiece{0x3000, 0},        // no slots: ignored entirely
    };

    const auto merged = MergeWornPieces(pieces);

    CHECK(merged.items.size() == 2);
    CHECK(merged.items[0].formId == 0x1000);
    CHECK(merged.items[0].slotMask == (bodySlot | (1U << 16)));
    CHECK(merged.items[1].formId == 0x2000);
    CHECK(merged.faceVisible);
  }
  {
    // A helmet taking the head slot hides the face; a circlet alone does not.
    const std::array helmet = {WornPiece{0x4000, BipedSlot::Head}};
    const std::array hood = {
        WornPiece{0x5000, BipedSlot::Hair | BipedSlot::Circlet}};
    const std::array circlet = {WornPiece{0x6000, BipedSlot::Circlet}};
    const std::array unreadable = {WornPiece{0, BipedSlot::Head}};

    CHECK(!MergeWornPieces(helmet).faceVisible);
    CHECK(!MergeWornPieces(hood).faceVisible);
    CHECK(MergeWornPieces(circlet).faceVisible);
    CHECK(MergeWornPieces(unreadable).faceVisible);
    CHECK(MergeWornPieces({}).faceVisible);
    CHECK(MergeWornPieces({}).items.empty());
  }
  {
    // Technical decapitation slots never reach the visual signature.
    const std::array technicalOnly = {
        WornPiece{0x7000, SkyrimRemember::technicalSlots}};
    CHECK(MergeWornPieces(technicalOnly).items.empty());

    // A piece mixing visual and technical slots keeps only the visual bits.
    const std::array mixed = {
        WornPiece{0x8000, bodySlot | BipedSlot::DecapitateHead}};
    const auto mergedMixed = MergeWornPieces(mixed);
    CHECK(mergedMixed.items.size() == 1);
    CHECK(mergedMixed.items[0].slotMask == bodySlot);

    // A helmet that also carries kDecapitateHead still hides the face.
    const std::array technicalHelmet = {
        WornPiece{0x9000, BipedSlot::Head | BipedSlot::Decapitate}};
    const auto mergedHelmet = MergeWornPieces(technicalHelmet);
    CHECK(mergedHelmet.items.size() == 1);
    CHECK(mergedHelmet.items[0].slotMask == BipedSlot::Head);
    CHECK(!mergedHelmet.faceVisible);

    // Bits 10 and 11 are tail and long hair, not technical slots.
    constexpr std::uint32_t tailAndLongHair = (1U << 10) | (1U << 11);
    const std::array tail = {WornPiece{0xA000, tailAndLongHair}};
    const auto mergedTail = MergeWornPieces(tail);
    CHECK(mergedTail.items.size() == 1);
    CHECK(mergedTail.items[0].slotMask == tailAndLongHair);
  }

  // Profile policy: the half of classification that needs no game.
  {
    using SkyrimRemember::IsProfileEnabled;
    using SkyrimRemember::NpcProfile;
    using SkyrimRemember::npcProfileCount;
    using SkyrimRemember::ProfileName;

    // Only Guard takes part in observation at this stage.
    CHECK(IsProfileEnabled(NpcProfile::Guard));
    CHECK(!IsProfileEnabled(NpcProfile::Follower));
    CHECK(!IsProfileEnabled(NpcProfile::Citizen));
    CHECK(!IsProfileEnabled(NpcProfile::Merchant));
    CHECK(!IsProfileEnabled(NpcProfile::Generic));

    // Every profile is nameable and every enum value stays inside the range the
    // metrics array is sized by.
    constexpr NpcProfile every[] = {NpcProfile::Guard, NpcProfile::Follower,
                                    NpcProfile::Citizen, NpcProfile::Merchant,
                                    NpcProfile::Generic};
    CHECK(std::size(every) == npcProfileCount);
    for (const auto profile : every) {
      CHECK(static_cast<std::size_t>(profile) < npcProfileCount);
      CHECK(ProfileName(profile) != nullptr);
      CHECK(std::strlen(ProfileName(profile)) > 0);
    }
    CHECK(std::strcmp(ProfileName(NpcProfile::Guard), "Guard") == 0);
    CHECK(std::strcmp(ProfileName(NpcProfile::Merchant), "Merchant") == 0);
  }

  CHECK(skyrim_remember_initialize(Log));

  CHECK(logged);

  using Context =
      std::unique_ptr<MemoryEngine, decltype(&skyrim_remember_destroy)>;

  Context engine(skyrim_remember_create(), skyrim_remember_destroy);

  CHECK(engine);

  MemorySnapshot snapshot{};
  Slot(snapshot, Axis::Recognition) = 1.0f;
  Slot(snapshot, Axis::Respect) = 4.0f;
  snapshot.lastDecayGameDays = 5.0;

  CHECK(skyrim_remember_get(engine.get(), 123, 10.0, &snapshot) ==
        BridgeStatus::NotFound);

  // A failed read must leave every byte of the caller's storage alone.
  CHECK(Slot(snapshot, Axis::Recognition) == 1.0f &&
        Slot(snapshot, Axis::Respect) == 4.0f &&
        snapshot.lastDecayGameDays == 5.0);

  CHECK(skyrim_remember_register(engine.get(), 0, 10.0) ==
        BridgeStatus::InvalidId);
  CHECK(skyrim_remember_register(engine.get(), 123, 10.0) ==
        BridgeStatus::Created);

  MemoryDelta delta{};
  Slot(delta, Axis::Fear) = 30.0f;
  Slot(delta, Axis::Respect) = 50.0f;
  CHECK(skyrim_remember_apply_delta(engine.get(), 123, 10.0, delta,
                                    &snapshot) == BridgeStatus::Ok);
  CHECK(Slot(snapshot, Axis::Fear) == 30.0f &&
        Slot(snapshot, Axis::Respect) == 50.0f);

  // A reserve slot the C++ side never wrote must come back as zero, and an
  // axis nobody touched must not have been disturbed by the delta.
  CHECK(Slot(snapshot, Axis::Approval) == 0.0f &&
        Slot(snapshot, Axis::Suspicion) == 0.0f);

  // Writing a follower axis through the same call works today, even though
  // nothing in the plugin does it yet.
  MemoryDelta followerDelta{};
  Slot(followerDelta, Axis::Approval) = 12.0f;
  CHECK(skyrim_remember_apply_delta(engine.get(), 123, 10.0, followerDelta,
                                    &snapshot) == BridgeStatus::Ok);
  CHECK(Slot(snapshot, Axis::Approval) == 12.0f &&
        Slot(snapshot, Axis::Fear) == 30.0f);
  Slot(followerDelta, Axis::Approval) = -12.0f;
  CHECK(skyrim_remember_apply_delta(engine.get(), 123, 10.0, followerDelta,
                                    &snapshot) == BridgeStatus::Ok);
  CHECK(Slot(snapshot, Axis::Approval) == 0.0f);

  const AppearanceItem outfit[] = {{0x1000, 1U << 2}, {0x2000, 1U << 3}};
  AppearanceResult appearance{};
  CHECK(skyrim_remember_evaluate_appearance(engine.get(), 123, outfit,
                                            std::size(outfit), 1, 0,
                                            &appearance) == BridgeStatus::Ok);
  CHECK(appearance.recognized == 1 && appearance.learned == 0 &&
        appearance.knownAppearances == 0);

  CHECK(skyrim_remember_observe_appearance(engine.get(), 123, 10.0, outfit,
                                           std::size(outfit), 0, 0,
                                           &appearance) == BridgeStatus::Ok);
  CHECK(appearance.appearanceMatched == 0 && appearance.recognized == 0 &&
        appearance.learned == 1 && appearance.itemCount == 2 &&
        appearance.knownAppearances == 1);

  CHECK(skyrim_remember_observe_appearance(engine.get(), 123, 10.0, outfit,
                                           std::size(outfit), 1, 0,
                                           &appearance) == BridgeStatus::Ok);
  CHECK(appearance.recognized == 1 && appearance.linkedByContinuity == 0);

  const AppearanceItem changedOutfit[] = {{0x3000, 1U << 2}};
  CHECK(skyrim_remember_observe_appearance(
            engine.get(), 123, 10.0, changedOutfit, std::size(changedOutfit), 0,
            0, &appearance) == BridgeStatus::Ok);
  CHECK(appearance.recognized == 0 && appearance.linkedByContinuity == 0 &&
        appearance.learned == 0);

  CHECK(skyrim_remember_observe_appearance(
            engine.get(), 123, 10.0, changedOutfit, std::size(changedOutfit), 0,
            1, &appearance) == BridgeStatus::Ok);
  CHECK(appearance.recognized == 1 && appearance.linkedByContinuity == 1 &&
        appearance.learned == 1);

  std::size_t length = 0;
  CHECK(skyrim_remember_serialize(engine.get(), nullptr, 0, &length) ==
        BridgeStatus::Ok);
  CHECK(length > 0);
  std::vector<std::uint8_t> bytes(length);
  CHECK(skyrim_remember_serialize(engine.get(), bytes.data(), bytes.size(),
                                  &length) == BridgeStatus::Ok);

  CHECK(skyrim_remember_clear(engine.get()) == BridgeStatus::Ok);
  PersistenceStats stats{};
  CHECK(skyrim_remember_deserialize(engine.get(), bytes.data(), bytes.size(),
                                    ResolveForm, nullptr,
                                    &stats) == BridgeStatus::Ok);
  CHECK(stats.loaded == 1 && stats.unresolved == 0);
  CHECK(skyrim_remember_get(engine.get(), 123, 10.0, &snapshot) ==
        BridgeStatus::Ok);
  CHECK(Slot(snapshot, Axis::Fear) == 30.0f &&
        Slot(snapshot, Axis::Respect) == 50.0f);
  CHECK(skyrim_remember_register(engine.get(), 123, 10.0) == BridgeStatus::Ok);
  CHECK(skyrim_remember_get(engine.get(), 123, 10.0, &snapshot) ==
        BridgeStatus::Ok);

  CHECK(Slot(snapshot, Axis::Recognition) == 0.0f &&
        Slot(snapshot, Axis::Suspicion) == 0.0f &&
        Slot(snapshot, Axis::Fear) == 30.0f &&
        Slot(snapshot, Axis::Respect) == 50.0f);

  CHECK(skyrim_remember_get(engine.get(), 456, 10.0, &snapshot) ==
        BridgeStatus::NotFound);

  CHECK(skyrim_remember_get(engine.get(), 123, 10.0, nullptr) ==
        BridgeStatus::InvalidArgument);

  CHECK(skyrim_remember_clear(engine.get()) == BridgeStatus::Ok);

  CHECK(skyrim_remember_get(engine.get(), 123, 10.0, &snapshot) ==
        BridgeStatus::NotFound);

  CHECK(skyrim_remember_register(engine.get(), 123, 10.0) ==
        BridgeStatus::Created);

  std::puts("C++/Rust bridge smoke test passed");
}
