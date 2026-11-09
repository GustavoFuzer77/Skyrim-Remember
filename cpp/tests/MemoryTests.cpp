#include "Appearance.h"
#include "GuardReactionAdapter.h"
#include "Memory.h"
#include "NpcProfile.h"
#include "RuleBook.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// Everything here runs without Skyrim: the memory model, recognition, rules and
// persistence are plain C++, and the host-side policy that does not read the
// game (outfit merging, profiles, guard reactions, notices) is exercised too.
// CHECK stays active in Release, unlike assert.
#define CHECK(condition)                                                                                                                                       \
  do {                                                                                                                                                         \
    if (!(condition)) {                                                                                                                                        \
      std::fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #condition);                                                                                   \
      return 1;                                                                                                                                                \
    }                                                                                                                                                          \
  } while (false)

namespace {

using namespace SkyrimRemember;

namespace Biped = SkyrimRemember::BipedSlot;

constexpr std::uint32_t bodySlot = 1U << 2;
constexpr std::uint32_t handsSlot = 1U << 3;

bool Near(float left, float right, float tolerance = 0.0001F) {
  return std::fabs(left - right) < tolerance;
}

float Level(const MemorySnapshot &snapshot, Axis axis) {
  return snapshot.axes[AxisSlot(axis)];
}

float &Slot(MemoryDelta &delta, Axis axis) {
  return delta.axes[AxisSlot(axis)];
}

AppearanceItem Item(std::uint32_t formId, std::uint32_t slotMask = bodySlot) {
  return {formId, slotMask};
}

// One memory at a game time, the way the host registers an observer.
void Know(MemoryStore &store, std::uint32_t id, double gameDays = 1.0) {
  store.Register(id, gameDays);
}

AppearanceResult Observe(MemoryStore &store, std::uint32_t id, std::vector<AppearanceItem> items, bool face, bool continuity, double gameDays) {
  AppearanceResult result;
  if (store.ObserveAppearance(id, gameDays, items, face, continuity, result) != MemoryStatus::Ok) {
    std::fputs("ObserveAppearance did not return Ok\n", stderr);
    std::abort();
  }
  return result;
}

float Current(MemoryStore &store, std::uint32_t id, Axis axis, double gameDays = 1.0) {
  MemorySnapshot snapshot;
  if (store.Get(id, gameDays, snapshot) != MemoryStatus::Ok) {
    std::fputs("Get did not return Ok\n", stderr);
    std::abort();
  }
  return Level(snapshot, axis);
}

// The stored value, without bringing the memory forward in time first.
float Raw(MemoryStore &store, std::uint32_t id, Axis axis) {
  return store.Find(id).value().axes[AxisSlot(axis)];
}

// The wire format of the co-save payload, written independently of the code
// that reads it, so a malformed or older payload can be built by hand.
struct Payload {
  std::vector<std::uint8_t> bytes;

  template <typename T>
  Payload &Put(T value) {
    const auto offset = bytes.size();
    bytes.resize(offset + sizeof(T));
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
    return *this;
  }

  // One memory with the given axes and no outfits, as the current version stores it.
  Payload &Memory(std::uint32_t id, std::array<float, axisCapacity> axes, double lastDecay, std::optional<double> lastRecognition = std::nullopt) {
    Put(id);
    for (const auto axis : axes) {
      Put(axis);
    }
    Put(lastDecay);
    Put<std::uint8_t>(lastRecognition ? 1 : 0);
    if (lastRecognition) {
      Put(*lastRecognition);
    }
    return *this;
  }
};

std::vector<std::uint8_t> FromHex(std::string_view hex) {
  std::vector<std::uint8_t> bytes;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    bytes.push_back(static_cast<std::uint8_t>(std::stoi(std::string{hex.substr(index, 2)}, nullptr, 16)));
  }
  return bytes;
}

const FormIdResolver keepIds = [](std::uint32_t id) { return std::optional{id}; };

// ---------------------------------------------------------------------------
// Host-side policy that needs no game.

int OutfitCapture() {
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

  // A helmet taking the head slot hides the face; a circlet alone does not.
  const std::array helmet = {WornPiece{0x4000, Biped::Head}};
  const std::array hood = {WornPiece{0x5000, Biped::Hair | Biped::Circlet}};
  const std::array circlet = {WornPiece{0x6000, Biped::Circlet}};
  const std::array unreadable = {WornPiece{0, Biped::Head}};

  CHECK(!MergeWornPieces(helmet).faceVisible);
  CHECK(!MergeWornPieces(hood).faceVisible);
  CHECK(MergeWornPieces(circlet).faceVisible);
  CHECK(MergeWornPieces(unreadable).faceVisible);
  CHECK(MergeWornPieces({}).faceVisible);
  CHECK(MergeWornPieces({}).items.empty());

  // Technical decapitation slots never reach the visual signature.
  const std::array technicalOnly = {WornPiece{0x7000, technicalSlots}};
  CHECK(MergeWornPieces(technicalOnly).items.empty());

  // A piece mixing visual and technical slots keeps only the visual bits.
  const std::array mixed = {WornPiece{0x8000, bodySlot | Biped::DecapitateHead}};
  const auto mergedMixed = MergeWornPieces(mixed);
  CHECK(mergedMixed.items.size() == 1);
  CHECK(mergedMixed.items[0].slotMask == bodySlot);

  // A helmet that also carries kDecapitateHead still hides the face.
  const std::array technicalHelmet = {WornPiece{0x9000, Biped::Head | Biped::Decapitate}};
  const auto mergedHelmet = MergeWornPieces(technicalHelmet);
  CHECK(mergedHelmet.items.size() == 1);
  CHECK(mergedHelmet.items[0].slotMask == Biped::Head);
  CHECK(!mergedHelmet.faceVisible);

  // Bits 10 and 11 are tail and long hair, not technical slots.
  constexpr std::uint32_t tailAndLongHair = (1U << 10) | (1U << 11);
  const std::array tail = {WornPiece{0xA000, tailAndLongHair}};
  const auto mergedTail = MergeWornPieces(tail);
  CHECK(mergedTail.items.size() == 1);
  CHECK(mergedTail.items[0].slotMask == tailAndLongHair);
  return 0;
}

int ProfilePolicy() {
  // Appearance memory is universal even before every profile has reactions.
  CHECK(IsProfileEnabled(NpcProfile::Guard));
  CHECK(IsProfileEnabled(NpcProfile::Follower));
  CHECK(IsProfileEnabled(NpcProfile::Citizen));
  CHECK(IsProfileEnabled(NpcProfile::Merchant));
  CHECK(IsProfileEnabled(NpcProfile::Generic));

  // Every profile is nameable and every enum value stays inside the range the
  // metrics array is sized by.
  constexpr NpcProfile every[] = {NpcProfile::Guard, NpcProfile::Follower, NpcProfile::Citizen, NpcProfile::Merchant, NpcProfile::Generic};
  CHECK(std::size(every) == npcProfileCount);
  for (std::size_t index = 0; index < std::size(every); ++index) {
    CHECK(static_cast<std::size_t>(every[index]) == index);
    CHECK(ProfileName(every[index]) != nullptr);
    CHECK(std::strlen(ProfileName(every[index])) > 0);
  }
  CHECK(std::strcmp(ProfileName(NpcProfile::Guard), "Guard") == 0);
  CHECK(std::strcmp(ProfileName(NpcProfile::Merchant), "Merchant") == 0);
  return 0;
}

int GuardReactionPolicy() {
  // The first recognition of a session or an encounter is a reaction, a
  // stranger's first sighting is not.
  IdentityObservation identity{};
  identity.recognitionChanged = true;
  identity.appearance.recognized = true;
  CHECK(DecideGuardReaction(identity) == GuardReaction::Recognized);

  identity.newObserver = true;
  CHECK(DecideGuardReaction(identity) == GuardReaction::None);

  identity.newObserver = false;
  identity.appearance.recognized = false;
  CHECK(DecideGuardReaction(identity) == GuardReaction::Unrecognized);

  identity.recognitionChanged = false;
  CHECK(DecideGuardReaction(identity) == GuardReaction::None);
  return 0;
}

int GuardNotices() {
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
  CHECK(summary.recognized == 2 && summary.unrecognized == 0);
  CHECK(board.Tracked() == 2);

  // Inside the grouping interval nothing is shown, but nothing is lost.
  board.Record(4, GuardReaction::Recognized);
  CHECK(board.Flush(start + seconds{1}).Empty());
  summary = board.Flush(start + Notice::groupingInterval);
  CHECK(summary.recognized == 1);

  // Flicker around the threshold cancels out before it reaches the HUD.
  auto now = start + Notice::groupingInterval * 2;
  board.Record(1, GuardReaction::Unrecognized);
  board.Record(1, GuardReaction::Recognized);
  CHECK(board.Flush(now).Empty());

  // A real loss waits out the per-guard spacing, then is shown once.
  board.Record(2, GuardReaction::Unrecognized);
  CHECK(board.Flush(now).Empty());
  now = start + Notice::perGuardSpacing;
  summary = board.Flush(now);
  CHECK(summary.recognized == 0 && summary.unrecognized == 1);

  // Recognizing again in the same encounter window is not repeated...
  board.Record(1, GuardReaction::Recognized);
  CHECK(board.Flush(now + Notice::groupingInterval).Empty());

  // ...but a later encounter is announced again, and stale guards are pruned.
  now = start + Notice::repeatAfter + Notice::perGuardSpacing;
  board.Record(1, GuardReaction::Recognized);
  summary = board.Flush(now);
  CHECK(summary.recognized == 1);
  CHECK(board.Tracked() == 1);

  board.Reset();
  CHECK(board.Tracked() == 0);

  CHECK(FormatGuardNotice(GuardReaction::Recognized, 1) == "A guard recognizes you.");
  CHECK(FormatGuardNotice(GuardReaction::Recognized, 3) == "3 guards recognize you.");
  CHECK(FormatGuardNotice(GuardReaction::Unrecognized, 1) == "A guard no longer recognizes you.");
  CHECK(FormatGuardNotice(GuardReaction::Unrecognized, 2) == "2 guards no longer recognize you.");
  CHECK(FormatGuardNotice(GuardReaction::Recognized, 0).empty());
  return 0;
}

// ---------------------------------------------------------------------------
// Memory: axes, decay, registration.

int RegistrationAndStatuses() {
  MemoryStore store;
  MemorySnapshot snapshot;
  snapshot.axes[AxisSlot(Axis::Recognition)] = 1.0F;
  snapshot.axes[AxisSlot(Axis::Respect)] = 4.0F;
  snapshot.lastDecayGameDays = 5.0;

  CHECK(store.Get(123, 10.0, snapshot) == MemoryStatus::NotFound);
  // A failed read must leave every byte of the caller's storage alone.
  CHECK(Level(snapshot, Axis::Recognition) == 1.0F && Level(snapshot, Axis::Respect) == 4.0F && snapshot.lastDecayGameDays == 5.0);
  CHECK(store.Size() == 0); // querying never creates a memory

  CHECK(store.Register(0, 10.0) == MemoryStatus::InvalidId);
  CHECK(store.Get(0, 10.0, snapshot) == MemoryStatus::InvalidId);
  CHECK(store.Register(123, -1.0) == MemoryStatus::InvalidArgument);
  CHECK(store.Register(123, std::nan("")) == MemoryStatus::InvalidArgument);
  CHECK(store.Register(123, 10.0) == MemoryStatus::Created);
  CHECK(store.Register(123, 10.0) == MemoryStatus::Ok);
  CHECK(store.Size() == 1);

  // A fresh memory is all zeros, stamped with the game time it was made at.
  CHECK(store.Get(123, 10.0, snapshot) == MemoryStatus::Ok);
  for (const auto axis : snapshot.axes) {
    CHECK(axis == 0.0F);
  }
  CHECK(snapshot.lastDecayGameDays == 10.0);

  // Actors are isolated from each other.
  MemoryDelta delta{};
  Slot(delta, Axis::Suspicion) = 35.0F;
  CHECK(store.ApplyDelta(123, 10.0, delta, snapshot) == MemoryStatus::Ok);
  CHECK(store.Register(456, 10.0) == MemoryStatus::Created);
  CHECK(store.Get(456, 10.0, snapshot) == MemoryStatus::Ok);
  CHECK(Level(snapshot, Axis::Suspicion) == 0.0F);
  CHECK(Current(store, 123, Axis::Suspicion, 10.0) == 35.0F);

  store.Clear();
  CHECK(store.Size() == 0);
  CHECK(store.Get(123, 10.0, snapshot) == MemoryStatus::NotFound);
  return 0;
}

int DeltasClampAndAxesAreIndependent() {
  MemoryStore store;
  MemorySnapshot snapshot;
  MemoryDelta delta{};
  Slot(delta, Axis::Fear) = 30.0F;
  Slot(delta, Axis::Respect) = 50.0F;
  CHECK(store.ApplyDelta(123, 10.0, delta, snapshot) == MemoryStatus::Ok);
  CHECK(Level(snapshot, Axis::Fear) == 30.0F && Level(snapshot, Axis::Respect) == 50.0F);
  // An axis nobody touched stays at zero, and so does a reserve slot.
  CHECK(Level(snapshot, Axis::Approval) == 0.0F && Level(snapshot, Axis::Suspicion) == 0.0F);
  CHECK(snapshot.axes[axisCapacity - 1] == 0.0F);

  // A follower axis works through the same call even though nothing uses it yet.
  MemoryDelta follower{};
  Slot(follower, Axis::Approval) = 12.0F;
  CHECK(store.ApplyDelta(123, 10.0, follower, snapshot) == MemoryStatus::Ok);
  CHECK(Level(snapshot, Axis::Approval) == 12.0F && Level(snapshot, Axis::Fear) == 30.0F);
  Slot(follower, Axis::Approval) = -12.0F;
  CHECK(store.ApplyDelta(123, 10.0, follower, snapshot) == MemoryStatus::Ok);
  CHECK(Level(snapshot, Axis::Approval) == 0.0F);

  // Every axis clamps on its own, at both ends.
  MemoryDelta extremes{};
  Slot(extremes, Axis::Fear) = 200.0F;
  Slot(extremes, Axis::Loyalty) = -5.0F;
  Slot(extremes, Axis::Trust) = 30.0F;
  CHECK(store.ApplyDelta(123, 10.0, extremes, snapshot) == MemoryStatus::Ok);
  CHECK(Level(snapshot, Axis::Fear) == 100.0F && Level(snapshot, Axis::Loyalty) == 0.0F && Level(snapshot, Axis::Trust) == 30.0F);
  CHECK(Level(snapshot, Axis::Respect) == 50.0F);

  // Invalid arguments are refused without touching the output or the memory.
  MemoryDelta poisoned{};
  Slot(poisoned, Axis::Fear) = std::nanf("");
  const auto before = snapshot;
  CHECK(store.ApplyDelta(123, 10.0, poisoned, snapshot) == MemoryStatus::InvalidArgument);
  CHECK(store.ApplyDelta(123, -1.0, delta, snapshot) == MemoryStatus::InvalidArgument);
  CHECK(store.ApplyDelta(0, 10.0, delta, snapshot) == MemoryStatus::InvalidId);
  CHECK(Level(snapshot, Axis::Fear) == Level(before, Axis::Fear));
  CHECK(store.Size() == 1);
  return 0;
}

int NamedAxesFitTheReserve() {
  CHECK(namedAxes.size() <= axisCapacity);
  for (std::size_t slot = 0; slot < namedAxes.size(); ++slot) {
    CHECK(AxisSlot(namedAxes[slot]) == slot);
    CHECK(std::strlen(AxisName(namedAxes[slot])) > 0);
  }
  // These numbers are persisted: moving one reinterprets every saved memory.
  CHECK(AxisSlot(Axis::Recognition) == 0 && AxisSlot(Axis::Suspicion) == 1 && AxisSlot(Axis::Fear) == 2 && AxisSlot(Axis::Respect) == 3);
  CHECK(AxisSlot(Axis::Approval) == 4 && AxisSlot(Axis::Loyalty) == 5 && AxisSlot(Axis::Resentment) == 6 && AxisSlot(Axis::Trust) == 7);
  return 0;
}

int DecayUsesHalfLives() {
  // Fear fades with a 72-hour half-life: three game days halve it.
  MemoryStore store;
  MemoryDelta delta{};
  Slot(delta, Axis::Fear) = 80.0F;
  MemorySnapshot snapshot;
  CHECK(store.ApplyDelta(123, 100.0, delta, snapshot) == MemoryStatus::Ok);
  CHECK(store.Get(123, 103.0, snapshot) == MemoryStatus::Ok);
  CHECK(Near(Level(snapshot, Axis::Fear), 40.0F));
  CHECK(snapshot.lastDecayGameDays == 103.0);

  // Time running backwards neither decays nor rewinds the stamp.
  CHECK(store.Get(123, 90.0, snapshot) == MemoryStatus::Ok);
  CHECK(Near(Level(snapshot, Axis::Fear), 40.0F));
  CHECK(snapshot.lastDecayGameDays == 103.0);

  // Registering an existing memory brings it forward without changing it.
  CHECK(store.Register(123, 106.0) == MemoryStatus::Ok);
  CHECK(Near(Current(store, 123, Axis::Fear, 106.0), 20.0F));

  // A reserve slot with no half-life holds its value while tuned axes fade.
  MemoryStore fresh;
  MemoryDelta mixed{};
  Slot(mixed, Axis::Approval) = 60.0F;
  Slot(mixed, Axis::Fear) = 80.0F;
  CHECK(fresh.ApplyDelta(1, 1.0, mixed, snapshot) == MemoryStatus::Ok);
  CHECK(fresh.Get(1, 4.0, snapshot) == MemoryStatus::Ok);
  CHECK(Level(snapshot, Axis::Approval) == 60.0F);
  CHECK(Level(snapshot, Axis::Fear) < 80.0F);

  // The store decays by the tuning it was built with, not by the defaults.
  Tuning quick;
  quick.decay.hours[AxisSlot(Axis::Fear)] = 24.0;
  MemoryStore custom(quick);
  MemoryDelta fear{};
  Slot(fear, Axis::Fear) = 80.0F;
  CHECK(custom.ApplyDelta(1, 1.0, fear, snapshot) == MemoryStatus::Ok);
  CHECK(custom.Get(1, 2.0, snapshot) == MemoryStatus::Ok);
  CHECK(Near(Level(snapshot, Axis::Fear), 40.0F));
  return 0;
}

int ThresholdAndWeights() {
  const RecognitionTuning recognition;
  CHECK(Near(recognition.Threshold(0.0F), 0.70F));
  CHECK(Near(recognition.Threshold(100.0F), 0.50F));
  CHECK(Near(recognition.Threshold(50.0F), 0.60F));
  // Out-of-range recognition stays inside the band.
  CHECK(Near(recognition.Threshold(-50.0F), 0.70F));
  CHECK(Near(recognition.Threshold(500.0F), 0.50F));

  const AppearanceWeights weights;
  CHECK(weights.Get(AppearanceCategory::Head) == weights.head);
  CHECK(weights.Get(AppearanceCategory::Body) == weights.body);
  CHECK(weights.Get(AppearanceCategory::Hands) == weights.hands);
  CHECK(weights.Get(AppearanceCategory::Feet) == weights.feet);
  CHECK(weights.Get(AppearanceCategory::Shield) == weights.shield);
  CHECK(weights.Get(AppearanceCategory::Accessories) == weights.accessories);
  return 0;
}

// ---------------------------------------------------------------------------
// Appearance and recognition.

int SimilarityIsWeightedBySlotGroup() {
  const AppearanceWeights weights;
  CHECK(OutfitSimilarity({}, {}, weights) == 1.0F); // two empty outfits are identical

  const std::array left = {Item(10, bodySlot), Item(20, handsSlot)};
  const std::array right = {Item(10, bodySlot), Item(30, handsSlot)};
  // Matching body weight 0.35 over applicable body+hands weight 0.45.
  CHECK(Near(OutfitSimilarity(left, right, weights), 0.35F / 0.45F));

  AppearanceWeights heavyHands;
  heavyHands.hands = 0.35F;
  CHECK(Near(OutfitSimilarity(left, right, heavyHands), 0.5F));
  return 0;
}

int NormalizingMergesAndStrips() {
  std::vector<AppearanceItem> items = {Item(20, 1U << 3), Item(0, 1U << 2), Item(10, 1U << 2), Item(10, 1U << 16), Item(30, 0)};
  NormalizeItems(items);
  CHECK((items == std::vector{Item(10, (1U << 2) | (1U << 16)), Item(20, 1U << 3)}));

  // Technical bits leave the signature: a piece with only those disappears, a
  // mixed piece keeps its visual bits, and tail/long hair are not technical.
  std::vector<AppearanceItem> technical = {Item(10, technicalSlots), Item(20, (1U << 2) | (1U << 20)), Item(30, 1U << 10)};
  NormalizeItems(technical);
  CHECK((technical == std::vector{Item(20, 1U << 2), Item(30, 1U << 10)}));

  std::vector<AppearanceItem> tail = {Item(10, (1U << 10) | (1U << 11))};
  NormalizeItems(tail);
  CHECK((tail == std::vector{Item(10, (1U << 10) | (1U << 11))}));
  return 0;
}

int CoveredFaceNeedsAMatchingOutfit() {
  MemoryStore store;
  Know(store, 123);

  const auto first = Observe(store, 123, {Item(10)}, false, false, 1.0);
  CHECK(!first.appearanceMatched && !first.recognized && first.learned);

  const auto same = Observe(store, 123, {Item(10)}, false, false, 1.1);
  CHECK(same.appearanceMatched && !same.recognized && !same.learned);
  CHECK(same.outfitSimilarity == 1.0F);

  const auto disguise = Observe(store, 123, {Item(20)}, false, false, 1.2);
  CHECK(!disguise.recognized);
  CHECK(disguise.identityConfidence == 0.0F);
  CHECK(disguise.knownAppearances == 1);
  return 0;
}

int AVisibleFaceConnectsANewOutfit() {
  MemoryStore store;
  Know(store, 123);
  Observe(store, 123, {Item(10)}, true, false, 1.0);

  const auto changed = Observe(store, 123, {Item(20)}, true, false, 1.1);
  CHECK(changed.recognized && changed.learned);
  CHECK(changed.outfitSimilarity == 0.0F && changed.identityConfidence == 1.0F);
  CHECK(changed.knownAppearances == 2);
  CHECK(changed.faceVisible);
  return 0;
}

int RevealingTheFaceLinksAnAnonymousOutfit() {
  MemoryStore store;
  Know(store, 123);

  CHECK(!Observe(store, 123, {Item(10)}, false, false, 1.0).recognized);

  const auto revealed = Observe(store, 123, {Item(10)}, true, false, 1.1);
  CHECK(revealed.recognized && revealed.appearanceMatched);

  const auto hiddenAgain = Observe(store, 123, {Item(10)}, false, false, 1.2);
  CHECK(hiddenAgain.recognized && hiddenAgain.appearanceMatched);
  return 0;
}

int VisualContinuity() {
  {
    // Uninterrupted contact links a completely different hidden outfit.
    MemoryStore store;
    Know(store, 123);
    Observe(store, 123, {Item(10)}, true, false, 1.0);

    const auto changed = Observe(store, 123, {Item(20)}, false, true, 1.1);
    CHECK(changed.recognized && changed.linkedByContinuity && changed.learned);
    CHECK(changed.outfitSimilarity == 0.0F);

    const auto seenLater = Observe(store, 123, {Item(20)}, false, false, 1.2);
    CHECK(seenLater.recognized && !seenLater.linkedByContinuity);
  }
  {
    // Once contact is lost, a hidden change is a stranger.
    MemoryStore store;
    Know(store, 123);
    Observe(store, 123, {Item(10)}, true, false, 1.0);

    const auto changed = Observe(store, 123, {Item(20)}, false, false, 1.1);
    CHECK(!changed.recognized && !changed.linkedByContinuity && !changed.learned);
  }
  {
    // Host-verified contact can establish identity on its own.
    MemoryStore store;
    Know(store, 123);
    const auto hidden = Observe(store, 123, {Item(10)}, false, true, 1.0);
    CHECK(hidden.recognized && hidden.linkedByContinuity);
  }
  return 0;
}

int EvaluatingHasNoSideEffects() {
  MemoryStore store;
  Know(store, 123);

  const std::array outfit = {Item(10)};
  AppearanceResult evaluation;
  CHECK(store.EvaluateAppearance(123, outfit, true, false, evaluation) == MemoryStatus::Ok);
  CHECK(evaluation.recognized && !evaluation.learned && evaluation.knownAppearances == 0);
  CHECK(store.KnownAppearances(123).empty());

  // Evaluation reports the normalized item count, not the raw one.
  const std::array raw = {Item(10, bodySlot), Item(10, handsSlot), Item(0, bodySlot), Item(20, technicalSlots)};
  CHECK(store.EvaluateAppearance(123, raw, true, false, evaluation) == MemoryStatus::Ok);
  CHECK(evaluation.itemCount == 1);

  // It does not create the memory, either.
  CHECK(store.EvaluateAppearance(999, outfit, true, false, evaluation) == MemoryStatus::NotFound);
  CHECK(store.Size() == 1);
  CHECK(store.EvaluateAppearance(0, outfit, true, false, evaluation) == MemoryStatus::InvalidId);
  return 0;
}

int ThresholdChangesWithFamiliarity() {
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
    CHECK(store.Deserialize(payload.bytes, keepIds, stats) == MemoryStatus::Ok);

    const auto result = Observe(store, 123, helmetChanged, false, false, 1.1);
    CHECK(Near(result.outfitSimilarity, 0.7058823F));
    CHECK(Near(result.recognitionThreshold, 0.70F));
    CHECK(result.recognized && result.learned);
  }
  {
    // Full familiarity lowers the bar to the floor.
    MemoryStore store;
    MemoryDelta delta{};
    Slot(delta, Axis::Recognition) = 100.0F;
    MemorySnapshot snapshot;
    store.ApplyDelta(123, 1.0, delta, snapshot);
    const auto first = Observe(store, 123, {Item(10)}, false, false, 1.0);
    CHECK(Near(first.recognitionThreshold, 0.50F));
  }
  return 0;
}

int FamiliarityIsEarnedNotFarmed() {
  MemoryStore store;
  Know(store, 123);

  const auto first = Observe(store, 123, {Item(10)}, true, false, 1.0);
  CHECK(Near(first.recognitionThreshold, 0.70F));
  CHECK(Raw(store, 123, Axis::Recognition) == 5.0F);

  // Polling a moment later gains nothing.
  const auto polled = Observe(store, 123, {Item(10)}, true, false, 1.1);
  CHECK(Near(polled.recognitionThreshold, 0.69F));
  CHECK(Raw(store, 123, Axis::Recognition) == 5.0F);

  // A properly spaced encounter does.
  Observe(store, 123, {Item(10)}, true, false, 1.25);
  CHECK(Raw(store, 123, Axis::Recognition) == 10.0F);

  // A covered face never builds facial familiarity, continuity or not.
  MemoryStore covered;
  Know(covered, 123);
  Observe(covered, 123, {Item(10)}, false, false, 1.0);
  Observe(covered, 123, {Item(10)}, false, true, 2.0);
  CHECK(Raw(covered, 123, Axis::Recognition) == 0.0F);
  return 0;
}

int OutfitCapacityAndRefresh() {
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
    CHECK(known.size() == 2);
    CHECK(std::ranges::none_of(known, [](const KnownAppearance &entry) { return entry.items[0].formId == 10; }));
  }
  {
    // Polling inside one game hour does not keep an outfit fresh...
    MemoryStore store;
    Know(store, 123);
    Observe(store, 123, {Item(10)}, true, false, 1.0);
    for (int tick = 1; tick <= 20; ++tick) {
      CHECK(!Observe(store, 123, {Item(10)}, true, false, 1.0 + tick * 0.0005).learned);
    }
    CHECK(store.KnownAppearances(123).size() == 1);
    CHECK(store.KnownAppearances(123)[0].lastSeenGameDays == 1.0);

    // ...but it is refreshed once the interval has really passed.
    Observe(store, 123, {Item(10)}, true, false, 1.5);
    CHECK(store.KnownAppearances(123)[0].lastSeenGameDays == 1.5);
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
    CHECK(known.size() == 2);
    CHECK(std::ranges::none_of(known, [](const KnownAppearance &entry) { return entry.items[0].formId == 10; }));
  }
  return 0;
}

int ItemLimitsAndArguments() {
  Tuning tuning;
  tuning.limits.maxAppearanceItems = 2;
  MemoryStore store(tuning);
  Know(store, 123);
  const std::array tooMany = {Item(10, bodySlot), Item(20, handsSlot), Item(30, 1U << 7)};
  AppearanceResult result;
  CHECK(store.ObserveAppearance(123, 1.0, tooMany, true, false, result) == MemoryStatus::InvalidArgument);
  CHECK(store.EvaluateAppearance(123, tooMany, true, false, result) == MemoryStatus::InvalidArgument);
  CHECK(store.KnownAppearances(123).empty());

  // The hard cap applies whatever the tuning says.
  MemoryStore plain;
  Know(plain, 123);
  std::vector<AppearanceItem> huge;
  for (std::uint32_t index = 1; index <= maxAppearanceItems + 1; ++index) {
    huge.push_back(Item(index));
  }
  CHECK(plain.ObserveAppearance(123, 1.0, huge, true, false, result) == MemoryStatus::InvalidArgument);

  CHECK(plain.ObserveAppearance(999, 1.0, {}, true, false, result) == MemoryStatus::NotFound);
  CHECK(plain.ObserveAppearance(0, 1.0, {}, true, false, result) == MemoryStatus::InvalidId);
  CHECK(plain.ObserveAppearance(123, -1.0, {}, true, false, result) == MemoryStatus::InvalidArgument);
  CHECK(plain.Size() == 1);
  return 0;
}

int StatusSequenceOfAWholeEncounter() {
  // The path a guard actually takes, end to end, through the public calls.
  MemoryStore store;
  Know(store, 123, 10.0);
  const std::array outfit = {Item(0x1000, 1U << 2), Item(0x2000, 1U << 3)};

  AppearanceResult result;
  CHECK(store.EvaluateAppearance(123, outfit, true, false, result) == MemoryStatus::Ok);
  CHECK(result.recognized && !result.learned && result.knownAppearances == 0);

  CHECK(store.ObserveAppearance(123, 10.0, outfit, false, false, result) == MemoryStatus::Ok);
  CHECK(!result.appearanceMatched && !result.recognized && result.learned && result.itemCount == 2 && result.knownAppearances == 1);

  CHECK(store.ObserveAppearance(123, 10.0, outfit, true, false, result) == MemoryStatus::Ok);
  CHECK(result.recognized && !result.linkedByContinuity);

  const std::array changed = {Item(0x3000, 1U << 2)};
  CHECK(store.ObserveAppearance(123, 10.0, changed, false, false, result) == MemoryStatus::Ok);
  CHECK(!result.recognized && !result.linkedByContinuity && !result.learned);

  CHECK(store.ObserveAppearance(123, 10.0, changed, false, true, result) == MemoryStatus::Ok);
  CHECK(result.recognized && result.linkedByContinuity && result.learned);
  return 0;
}

// ---------------------------------------------------------------------------
// Persistence.

int RoundTrip() {
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
  CHECK(first == store.Serialize()); // deterministic
  CHECK(!first.empty());

  store.Clear();
  PersistenceStats stats;
  CHECK(store.Deserialize(first, keepIds, stats) == MemoryStatus::Ok);
  CHECK(stats.loaded == 2 && stats.unresolved == 0);
  CHECK(store.Size() == 2);
  CHECK(Current(store, 0x123, Axis::Respect, 11.0) == 50.0F);
  CHECK(Current(store, 0x456, Axis::Fear, 10.0) == 30.0F);
  const auto memory = store.Find(0x123);
  CHECK(memory && memory->lastDecayGameDays == 11.0 && memory->lastRecognitionGameDays == 11.0);
  CHECK(store.KnownAppearances(0x123).at(0).items.at(0).formId == 0x1000);
  CHECK(store.Serialize() == first); // and stable across a reload

  // The reload kept the familiarity gained from the visible face.
  CHECK(store.Register(0x123, 11.0) == MemoryStatus::Ok);
  CHECK(Current(store, 0x123, Axis::Recognition, 11.0) == 5.0F);
  return 0;
}

// A payload the Rust core wrote (bincode 1.x): two NPCs, the second with an
// outfit seen with a visible face. Saves from before the core moved to C++ must
// keep loading.
int ASavedFromTheRustCoreStillLoads() {
  constexpr std::string_view golden =
      "050000000200000000000000230100000000a0400000f040000000000000484200000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
      "000000000000000000000026400100000000000026400100000000000000020000000000000000100000040000000020000018000000010100000000000026405604000000000000000000"
      "000000f041000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002440000000000000000000";
  const auto bytes = FromHex(golden);
  CHECK(bytes.size() == 224);

  MemoryStore store;
  PersistenceStats stats;
  CHECK(store.Deserialize(bytes, keepIds, stats) == MemoryStatus::Ok);
  CHECK(stats.loaded == 2 && stats.unresolved == 0);

  MemorySnapshot snapshot;
  CHECK(store.Get(0x123, 11.0, snapshot) == MemoryStatus::Ok);
  CHECK(Level(snapshot, Axis::Recognition) == 5.0F && Level(snapshot, Axis::Suspicion) == 7.5F && Level(snapshot, Axis::Respect) == 50.0F);
  CHECK(store.Get(0x456, 10.0, snapshot) == MemoryStatus::Ok);
  CHECK(Level(snapshot, Axis::Fear) == 30.0F);
  CHECK(store.Find(0x123)->lastRecognitionGameDays == 11.0);
  CHECK(!store.Find(0x456)->lastRecognitionGameDays);

  const auto known = store.KnownAppearances(0x123);
  CHECK(known.size() == 1 && known[0].faceVisible && known[0].identityKnown && known[0].lastSeenGameDays == 11.0);
  CHECK((known[0].items == std::vector{Item(0x1000, 1U << 2), Item(0x2000, (1U << 3) | (1U << 4))}));

  // And writing it back reproduces the bytes exactly.
  CHECK(store.Serialize() == bytes);
  return 0;
}

int FormsAreRemappedAndMissingOnesDropped() {
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
  CHECK(store.Deserialize(bytes, remap, stats) == MemoryStatus::Ok);
  CHECK(stats.loaded == 1 && stats.unresolved == 1);
  CHECK(store.Size() == 1 && !store.Find(10));
  CHECK(Current(store, 110, Axis::Fear) == 20.0F);
  const auto known = store.KnownAppearances(110);
  CHECK(known.size() == 1 && known[0].items.size() == 1 && known[0].items[0].formId == 120);

  // An NPC that no longer resolves is skipped and counted, not an error.
  store.Clear();
  CHECK(store.Deserialize(bytes, [](std::uint32_t) { return std::nullopt; }, stats) == MemoryStatus::Ok);
  CHECK(stats.loaded == 0 && stats.unresolved == 1 && store.Size() == 0);
  return 0;
}

int ASavedOutfitWithTechnicalBitsStillMatches() {
  // Restored the way an older save would have written it.
  Payload payload;
  payload.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(123, {}, 1.0);
  payload.Put<std::uint64_t>(1).Put<std::uint64_t>(1).Put<std::uint32_t>(10).Put<std::uint32_t>((1U << 2) | (1U << 20));
  payload.Put<std::uint8_t>(1).Put<std::uint8_t>(1).Put(1.0);

  MemoryStore store;
  PersistenceStats stats;
  CHECK(store.Deserialize(payload.bytes, keepIds, stats) == MemoryStatus::Ok);

  const auto seenAgain = Observe(store, 123, {Item(10, 1U << 2)}, true, false, 1.5);
  CHECK(seenAgain.outfitSimilarity == 1.0F && seenAgain.appearanceMatched);
  CHECK(!seenAgain.learned); // the same outfit must not be relearned
  CHECK(store.KnownAppearances(123).size() == 1);
  return 0;
}

int CorruptedPayloadsAreRejectedAtomically() {
  MemoryStore store;
  store.Register(777, 1.0);
  PersistenceStats stats;

  CHECK(store.Deserialize(std::vector<std::uint8_t>{1, 2, 3}, keepIds, stats) == MemoryStatus::InvalidData);
  CHECK(store.Deserialize({}, keepIds, stats) == MemoryStatus::InvalidData);
  CHECK(store.Deserialize(std::vector<std::uint8_t>{1, 2, 3}, nullptr, stats) == MemoryStatus::InvalidArgument);

  Payload unsupported;
  unsupported.Put<std::uint32_t>(6).Put<std::uint64_t>(0);
  CHECK(store.Deserialize(unsupported.bytes, keepIds, stats) == MemoryStatus::UnsupportedVersion);
  Payload older;
  older.Put<std::uint32_t>(4).Put<std::uint64_t>(0);
  CHECK(store.Deserialize(older.bytes, keepIds, stats) == MemoryStatus::UnsupportedVersion);

  // A NaN axis is not something the game could have produced.
  std::array<float, axisCapacity> nan{};
  nan[AxisSlot(Axis::Fear)] = std::nanf("");
  Payload invalid;
  invalid.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(10, nan, 1.0).Put<std::uint64_t>(0);
  CHECK(store.Deserialize(invalid.bytes, keepIds, stats) == MemoryStatus::InvalidData);

  // A zero reference, an out-of-range axis, an absurd outfit count, a truncated
  // payload: all rejected.
  Payload zeroId;
  zeroId.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(0, {}, 1.0).Put<std::uint64_t>(0);
  CHECK(store.Deserialize(zeroId.bytes, keepIds, stats) == MemoryStatus::InvalidData);

  std::array<float, axisCapacity> huge{};
  huge[AxisSlot(Axis::Respect)] = 101.0F;
  Payload outOfRange;
  outOfRange.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(10, huge, 1.0).Put<std::uint64_t>(0);
  CHECK(store.Deserialize(outOfRange.bytes, keepIds, stats) == MemoryStatus::InvalidData);

  Payload absurd;
  absurd.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(10, {}, 1.0).Put<std::uint64_t>(0xFFFFFFFFFFFFFFFFULL);
  CHECK(store.Deserialize(absurd.bytes, keepIds, stats) == MemoryStatus::InvalidData);

  Payload tooManyOutfits;
  tooManyOutfits.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(10, {}, 1.0).Put<std::uint64_t>(maxKnownAppearances + 1);
  for (std::size_t index = 0; index <= maxKnownAppearances; ++index) {
    tooManyOutfits.Put<std::uint64_t>(0).Put<std::uint8_t>(1).Put<std::uint8_t>(1).Put(1.0);
  }
  CHECK(store.Deserialize(tooManyOutfits.bytes, keepIds, stats) == MemoryStatus::InvalidData);

  Payload badBool;
  badBool.Put<std::uint32_t>(5).Put<std::uint64_t>(1).Memory(10, {}, 1.0).Put<std::uint64_t>(1).Put<std::uint64_t>(0).Put<std::uint8_t>(7).Put<std::uint8_t>(0).Put(1.0);
  CHECK(store.Deserialize(badBool.bytes, keepIds, stats) == MemoryStatus::InvalidData);

  auto truncated = MemoryStore{};
  truncated.Register(5, 1.0);
  auto bytes = truncated.Serialize();
  bytes.pop_back();
  CHECK(store.Deserialize(bytes, keepIds, stats) == MemoryStatus::InvalidData);

  // None of that disturbed what the store already held.
  CHECK(store.Size() == 1 && store.Find(777));
  return 0;
}

// ---------------------------------------------------------------------------
// Rules and impressions.

constexpr std::uint32_t banditCuirass = 0x0006F393;
constexpr std::uint32_t hideKeyword = 0x01000001;
constexpr std::uint32_t ironKeyword = 0x01000002;

constexpr std::string_view banditRules = R"({
  "impressions": [
    { "name": "bandit", "forms": ["0x06F393~Skyrim.esm"], "projects": { "banditry": 40 } },
    { "keywords": ["ArmorMaterialHide"], "projects": { "banditry": 15 } }
  ],
  "temperaments": {
    "guard": { "banditry": { "suspicion": 1.0 } },
    "citizen": { "banditry": { "suspicion": 0.5, "fear": 0.5 } }
  }
})";

// Binds the way a host would: the bandit cuirass to its FormID and the hide
// keyword to a made-up one.
void BindLikeAHost(RuleBook &rules) {
  for (std::size_t index = 0; index < rules.References().size(); ++index) {
    const auto &reference = rules.References()[index];
    std::uint32_t runtime = ironKeyword;
    if (reference.kind == RuleReferenceKind::Form) {
      runtime = reference.localFormId;
    } else if (reference.name == "ArmorMaterialHide") {
      runtime = hideKeyword;
    }
    rules.Bind(index, runtime);
  }
}

bool Loaded(RuleBook &rules, std::string_view json) {
  return !rules.LoadJson(json).has_value();
}

float Target(const ImpressionOutcome &outcome, Axis axis) {
  return outcome.targets[AxisSlot(axis)];
}

int TheShippedRuleFileIsValid() {
  std::ifstream input(SHIPPED_RULES_PATH, std::ios::binary);
  CHECK(input);
  const std::string json{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};

  RuleBook rules;
  const auto rejection = rules.LoadJson(json);
  if (rejection) {
    std::fprintf(stderr, "shipped rules rejected: %s\n", rejection->c_str());
  }
  CHECK(!rejection);
  CHECK(rules.ImpressionCount() >= 1);
  const RuleReference bandit{RuleReferenceKind::Form, "Skyrim.esm", banditCuirass};
  CHECK(std::ranges::find(rules.References(), bandit) != rules.References().end());
  return 0;
}

int ReferencesAreListedOnceInLoadOrder() {
  RuleBook rules;
  CHECK(Loaded(rules, R"({ "impressions": [
      { "keywords": ["A", "B"], "projects": { "x": 1 } },
      { "keywords": ["B"], "forms": ["800~Mod.esp"], "projects": { "x": 2 } }
    ] })"));

  const std::vector<RuleReference> expected = {
      {RuleReferenceKind::Keyword, "A", 0},
      {RuleReferenceKind::Keyword, "B", 0},
      {RuleReferenceKind::Form, "Mod.esp", 0x800},
  };
  CHECK(rules.References() == expected);
  CHECK(rules.ImpressionCount() == 2);
  return 0;
}

int RulesProjectThroughTemperaments() {
  RuleBook rules;
  CHECK(Loaded(rules, banditRules));

  // Nothing matches until the host binds a reference.
  const std::array<std::uint32_t, 1> cuirass = {banditCuirass};
  const auto unbound = rules.Evaluate(NpcProfile::Guard, cuirass);
  CHECK(unbound.matchedRules == 0 && Target(unbound, Axis::Suspicion) == 0.0F);
  CHECK(rules.BoundCount() == 0);

  BindLikeAHost(rules);
  CHECK(rules.BoundCount() == 2);

  const auto guard = rules.Evaluate(NpcProfile::Guard, cuirass);
  CHECK(Target(guard, Axis::Suspicion) == 40.0F && Target(guard, Axis::Fear) == 0.0F && guard.matchedRules == 1);

  const auto citizen = rules.Evaluate(NpcProfile::Citizen, cuirass);
  CHECK(Target(citizen, Axis::Suspicion) == 20.0F && Target(citizen, Axis::Fear) == 20.0F);

  // A profile without a temperament entry is unaffected, though the rules match.
  const std::array<std::uint32_t, 2> cuirassAndHide = {banditCuirass, hideKeyword};
  const auto follower = rules.Evaluate(NpcProfile::Follower, cuirassAndHide);
  CHECK(follower.matchedRules == 2);
  for (const auto target : follower.targets) {
    CHECK(target == 0.0F);
  }
  return 0;
}

int TheStrongestRuleWinsInsteadOfAddingUp() {
  RuleBook rules;
  CHECK(Loaded(rules, banditRules));
  BindLikeAHost(rules);

  const std::array<std::uint32_t, 2> hideOnly = {0x1234, hideKeyword};
  CHECK(Target(rules.Evaluate(NpcProfile::Guard, hideOnly), Axis::Suspicion) == 15.0F);

  const std::array<std::uint32_t, 2> both = {banditCuirass, hideKeyword};
  const auto outcome = rules.Evaluate(NpcProfile::Guard, both);
  CHECK(Target(outcome, Axis::Suspicion) == 40.0F && outcome.matchedRules == 2);

  const std::array<std::uint32_t, 2> unrelated = {0x1234, ironKeyword};
  const auto none = rules.Evaluate(NpcProfile::Guard, unrelated);
  CHECK(none.matchedRules == 0 && Target(none, Axis::Suspicion) == 0.0F);
  return 0;
}

int UnbindingStopsAReferenceMatching() {
  RuleBook rules;
  CHECK(Loaded(rules, banditRules));
  BindLikeAHost(rules);
  CHECK(rules.Bind(0, 0));

  const std::array<std::uint32_t, 1> cuirass = {banditCuirass};
  CHECK(Target(rules.Evaluate(NpcProfile::Guard, cuirass), Axis::Suspicion) == 0.0F);
  CHECK(rules.BoundCount() == 1);
  CHECK(!rules.Bind(99, 1));
  return 0;
}

int ALaterFileRetunesTemperaments() {
  RuleBook rules;
  CHECK(Loaded(rules, banditRules));
  BindLikeAHost(rules);
  CHECK(Loaded(rules, R"({ "temperaments": { "guard": { "banditry": { "fear": 0.5 } } } })"));

  const std::array<std::uint32_t, 1> cuirass = {banditCuirass};
  const auto guard = rules.Evaluate(NpcProfile::Guard, cuirass);
  CHECK(Target(guard, Axis::Suspicion) == 0.0F && Target(guard, Axis::Fear) == 20.0F);
  return 0;
}

int InvalidRuleFilesAreRejectedWhole() {
  struct Case {
    std::string_view json;
    std::string_view reason;
  };
  const std::array cases = {
      Case{"{ not json", "parse error"},
      Case{R"({ "impresions": [] })", "unknown field"},
      Case{R"([])", "expected an object"},
      Case{R"({ "impressions": [ { "projects": { "x": 1 } } ] })", "at least one keyword or form"},
      Case{R"({ "impressions": [ { "keywords": ["A"] } ] })", "missing field \"projects\""},
      Case{R"({ "impressions": [ { "keywords": ["A"], "projects": {} } ] })", "at least one projection"},
      Case{R"({ "impressions": [ { "keywords": ["A"], "projects": { "x": 150 } } ] })", "strength 150"},
      Case{R"({ "impressions": [ { "keywords": ["A"], "projects": { "x": -1 } } ] })", "between 0 and"},
      Case{R"({ "impressions": [ { "keywords": ["A"], "projects": { "x": "high" } } ] })", "expected a number"},
      Case{R"({ "impressions": [ { "keywords": [" "], "projects": { "x": 1 } } ] })", "empty keyword"},
      Case{R"({ "impressions": [ { "forms": ["0x06F393"], "projects": { "x": 1 } } ] })", "must look like"},
      Case{R"({ "impressions": [ { "forms": ["0xZZ~A.esp"], "projects": { "x": 1 } } ] })", "invalid hexadecimal"},
      Case{R"({ "impressions": [ { "forms": ["0x1000000~A.esp"], "projects": { "x": 1 } } ] })", "local id"},
      Case{R"({ "impressions": [ { "forms": ["0x0~A.esp"], "projects": { "x": 1 } } ] })", "local id"},
      Case{R"({ "impressions": [ { "forms": ["0x10~"], "projects": { "x": 1 } } ] })", "names no plugin"},
      Case{R"({ "temperaments": { "bandit": { "x": { "fear": 1 } } } })", "unknown profile \"bandit\""},
      Case{R"({ "temperaments": { "guard": { "x": { "anger": 1 } } } })", "unknown axis \"anger\""},
      Case{R"({ "temperaments": { "guard": { "x": { "fear": -1 } } } })", "must be zero or positive"},
      // A good entry followed by a bad one: the good one must not survive.
      Case{R"({ "impressions": [ { "keywords": ["Fine"], "projects": { "x": 1 } }, { "keywords": ["Bad"] } ] })", "projects"},
  };
  for (const auto &entry : cases) {
    RuleBook rules;
    CHECK(Loaded(rules, banditRules));
    BindLikeAHost(rules);
    const auto references = rules.References().size();
    const auto impressions = rules.ImpressionCount();

    const auto rejection = rules.LoadJson(entry.json);
    if (!rejection || rejection->find(entry.reason) == std::string::npos) {
      std::fprintf(stderr, "%.*s: expected \"%.*s\", got \"%s\"\n", static_cast<int>(entry.json.size()), entry.json.data(), static_cast<int>(entry.reason.size()),
                   entry.reason.data(), rejection ? rejection->c_str() : "(accepted)");
      return 1;
    }
    CHECK(rules.References().size() == references && rules.ImpressionCount() == impressions);
    const std::array<std::uint32_t, 1> cuirass = {banditCuirass};
    CHECK(Target(rules.Evaluate(NpcProfile::Guard, cuirass), Axis::Suspicion) == 40.0F);
  }

  // Empty, null and absent sections are fine; unrelated fields are not.
  RuleBook rules;
  CHECK(Loaded(rules, "{}"));
  CHECK(Loaded(rules, R"({ "impressions": [], "temperaments": {} })"));
  CHECK(Loaded(rules, R"({ "impressions": null, "temperaments": null })"));
  CHECK(Loaded(rules, R"({ "impressions": [ { "name": null, "keywords": null, "forms": ["0x10~A.esp"], "projects": { "x": 1 } } ] })"));
  CHECK(rules.References().size() == 1);
  return 0;
}

// The store's view of the same rules, driven the way ImpressionRules.cpp drives
// them: load, list references, bind the ones the game resolves, then observe.
int ImpressionsRaiseMemory() {
  MemoryStore store;
  std::string_view broken = R"({ "temperaments": { "guard": { "banditry": { "anger": 1 } } } })";
  const auto reason = store.LoadRules(broken);
  CHECK(reason && reason->find("unknown axis") != std::string::npos);
  CHECK(store.LoadRules("")); // empty is refused too
  CHECK(!store.LoadRules(banditRules));

  const auto summary = store.GetRulesSummary();
  CHECK(summary.impressions == 2 && summary.references == 2 && summary.boundReferences == 0);
  CHECK(!store.GetRuleReference(2));
  const auto keyword = store.GetRuleReference(1);
  CHECK(keyword && keyword->kind == RuleReferenceKind::Keyword && keyword->name == "ArmorMaterialHide");
  CHECK(store.BindRuleReference(1, hideKeyword) == MemoryStatus::Ok);
  CHECK(store.BindRuleReference(7, hideKeyword) == MemoryStatus::NotFound);
  CHECK(store.GetRulesSummary().boundReferences == 1);

  const std::array hideCuirass = {Item(0x13911)};
  const std::array<std::uint32_t, 1> keywords = {hideKeyword};
  ImpressionResult impression;

  // A memory must exist first.
  CHECK(store.ObserveImpression(123, 10.0, NpcProfile::Guard, hideCuirass, keywords, impression) == MemoryStatus::NotFound);
  Know(store, 123, 10.0);

  CHECK(store.ObserveImpression(123, 10.0, NpcProfile::Guard, hideCuirass, keywords, impression) == MemoryStatus::Ok);
  CHECK(impression.raisedAxes == (1U << AxisSlot(Axis::Suspicion)) && impression.matchedRules == 1);
  CHECK(impression.targets[AxisSlot(Axis::Suspicion)] == 15.0F);

  // Worn again on the next scan: the level holds and nothing is reported twice.
  for (int scan = 0; scan < 10; ++scan) {
    CHECK(store.ObserveImpression(123, 10.0, NpcProfile::Guard, hideCuirass, keywords, impression) == MemoryStatus::Ok);
    CHECK(impression.raisedAxes == 0);
  }
  CHECK(Current(store, 123, Axis::Suspicion, 10.0) == 15.0F);

  // A profile with no temperament for it feels nothing.
  CHECK(store.ObserveImpression(123, 10.0, NpcProfile::Follower, hideCuirass, keywords, impression) == MemoryStatus::Ok);
  CHECK(impression.targets[AxisSlot(Axis::Suspicion)] == 0.0F);

  // Bad arguments.
  CHECK(store.ObserveImpression(123, 10.0, static_cast<NpcProfile>(99), hideCuirass, keywords, impression) == MemoryStatus::InvalidArgument);
  CHECK(store.ObserveImpression(123, -1.0, NpcProfile::Guard, hideCuirass, keywords, impression) == MemoryStatus::InvalidArgument);
  CHECK(store.ObserveImpression(0, 10.0, NpcProfile::Guard, hideCuirass, keywords, impression) == MemoryStatus::InvalidId);
  const std::vector<std::uint32_t> tooManyKeywords(maxWornKeywords + 1, hideKeyword);
  CHECK(store.ObserveImpression(123, 10.0, NpcProfile::Guard, hideCuirass, tooManyKeywords, impression) == MemoryStatus::InvalidArgument);

  // Rules belong to the session: clearing memories leaves them bound.
  store.Clear();
  CHECK(store.GetRulesSummary().boundReferences == 1);
  return 0;
}

int ImpressionsNeverLowerAndTopUpQuietly() {
  MemoryStore store;
  CHECK(!store.LoadRules(banditRules));
  for (std::uint32_t index = 0; index < 2; ++index) {
    const auto reference = store.GetRuleReference(index);
    store.BindRuleReference(index, reference->kind == RuleReferenceKind::Form ? reference->localFormId : hideKeyword);
  }
  const std::array outfit = {Item(banditCuirass)};
  ImpressionResult impression;

  // Raises to the level, never accumulates.
  Know(store, 1, 1.0);
  CHECK(store.ObserveImpression(1, 1.0, NpcProfile::Guard, outfit, {}, impression) == MemoryStatus::Ok);
  CHECK(impression.raisedAxes == (1U << AxisSlot(Axis::Suspicion)));
  CHECK(Current(store, 1, Axis::Suspicion) == 40.0F);

  // One game hour of a 48-hour half-life is well under one point of decay: the
  // top-up is applied but is not news.
  CHECK(store.ObserveImpression(1, 1.0 + 1.0 / 24.0, NpcProfile::Guard, outfit, {}, impression) == MemoryStatus::Ok);
  CHECK(impression.raisedAxes == 0);
  CHECK(Current(store, 1, Axis::Suspicion, 1.0 + 1.0 / 24.0) == 40.0F);

  // A stronger feeling from an earlier event survives an outfit that implies less.
  Know(store, 2, 1.0);
  MemoryDelta delta{};
  Slot(delta, Axis::Suspicion) = 90.0F;
  MemorySnapshot snapshot;
  store.ApplyDelta(2, 1.0, delta, snapshot);
  CHECK(store.ObserveImpression(2, 1.0, NpcProfile::Guard, outfit, {}, impression) == MemoryStatus::Ok);
  CHECK(impression.raisedAxes == 0);
  CHECK(Current(store, 2, Axis::Suspicion) == 90.0F);
  return 0;
}

int ProfilesMirrorTheHostEnum() {
  // The rule file names and the host enum must agree on every profile.
  RuleBook rules;
  constexpr std::array<const char *, npcProfileCount> names = {"guard", "follower", "citizen", "merchant", "generic"};
  for (std::size_t index = 0; index < names.size(); ++index) {
    CHECK(Loaded(rules, std::string{R"({ "temperaments": { ")"} + names[index] + R"(": { "x": { "fear": 1 } } } })"));
    CHECK(static_cast<std::size_t>(static_cast<NpcProfile>(index)) == index);
  }
  CHECK(!Loaded(rules, R"({ "temperaments": { "hostile": {} } })"));
  return 0;
}

struct NamedTest {
  const char *name;
  std::function<int()> run;
};

} // namespace

int main() noexcept {
  try {
    const std::vector<NamedTest> tests = {
        {"OutfitCapture", OutfitCapture},
        {"ProfilePolicy", ProfilePolicy},
        {"GuardReactionPolicy", GuardReactionPolicy},
        {"GuardNotices", GuardNotices},
        {"RegistrationAndStatuses", RegistrationAndStatuses},
        {"DeltasClampAndAxesAreIndependent", DeltasClampAndAxesAreIndependent},
        {"NamedAxesFitTheReserve", NamedAxesFitTheReserve},
        {"DecayUsesHalfLives", DecayUsesHalfLives},
        {"ThresholdAndWeights", ThresholdAndWeights},
        {"SimilarityIsWeightedBySlotGroup", SimilarityIsWeightedBySlotGroup},
        {"NormalizingMergesAndStrips", NormalizingMergesAndStrips},
        {"CoveredFaceNeedsAMatchingOutfit", CoveredFaceNeedsAMatchingOutfit},
        {"AVisibleFaceConnectsANewOutfit", AVisibleFaceConnectsANewOutfit},
        {"RevealingTheFaceLinksAnAnonymousOutfit", RevealingTheFaceLinksAnAnonymousOutfit},
        {"VisualContinuity", VisualContinuity},
        {"EvaluatingHasNoSideEffects", EvaluatingHasNoSideEffects},
        {"ThresholdChangesWithFamiliarity", ThresholdChangesWithFamiliarity},
        {"FamiliarityIsEarnedNotFarmed", FamiliarityIsEarnedNotFarmed},
        {"OutfitCapacityAndRefresh", OutfitCapacityAndRefresh},
        {"ItemLimitsAndArguments", ItemLimitsAndArguments},
        {"StatusSequenceOfAWholeEncounter", StatusSequenceOfAWholeEncounter},
        {"RoundTrip", RoundTrip},
        {"ASavedFromTheRustCoreStillLoads", ASavedFromTheRustCoreStillLoads},
        {"FormsAreRemappedAndMissingOnesDropped", FormsAreRemappedAndMissingOnesDropped},
        {"ASavedOutfitWithTechnicalBitsStillMatches", ASavedOutfitWithTechnicalBitsStillMatches},
        {"CorruptedPayloadsAreRejectedAtomically", CorruptedPayloadsAreRejectedAtomically},
        {"TheShippedRuleFileIsValid", TheShippedRuleFileIsValid},
        {"ReferencesAreListedOnceInLoadOrder", ReferencesAreListedOnceInLoadOrder},
        {"RulesProjectThroughTemperaments", RulesProjectThroughTemperaments},
        {"TheStrongestRuleWinsInsteadOfAddingUp", TheStrongestRuleWinsInsteadOfAddingUp},
        {"UnbindingStopsAReferenceMatching", UnbindingStopsAReferenceMatching},
        {"ALaterFileRetunesTemperaments", ALaterFileRetunesTemperaments},
        {"InvalidRuleFilesAreRejectedWhole", InvalidRuleFilesAreRejectedWhole},
        {"ImpressionsRaiseMemory", ImpressionsRaiseMemory},
        {"ImpressionsNeverLowerAndTopUpQuietly", ImpressionsNeverLowerAndTopUpQuietly},
        {"ProfilesMirrorTheHostEnum", ProfilesMirrorTheHostEnum},
    };

    int failures = 0;
    for (const auto &test : tests) {
      if (test.run() != 0) {
        std::fprintf(stderr, "FAILED: %s\n", test.name);
        ++failures;
      }
    }
    if (failures != 0) {
      std::fprintf(stderr, "%d of %zu tests failed\n", failures, tests.size());
      return 1;
    }
    std::printf("All %zu memory tests passed\n", tests.size());
    return 0;
  } catch (...) {
    std::fputs("A memory test escaped with an exception\n", stderr);
    return 1;
  }
}
