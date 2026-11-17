// The memory model: registration, axes, decay and the recognition tuning.

#include "TestSupport.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

using namespace SkyrimRemember;
using namespace SkyrimRemember::Testing;

TEST_SUITE_BEGIN("memory model");

TEST_CASE("RegistrationAndStatuses") {
  MemoryStore store;
  MemorySnapshot snapshot;
  snapshot.axes[AxisSlot(Axis::Recognition)] = 1.0F;
  snapshot.axes[AxisSlot(Axis::Respect)] = 4.0F;
  snapshot.lastDecayGameDays = 5.0;

  REQUIRE(store.Get(123, 10.0, snapshot) == MemoryStatus::NotFound);
  // A failed read must leave every byte of the caller's storage alone.
  REQUIRE((Level(snapshot, Axis::Recognition) == 1.0F && Level(snapshot, Axis::Respect) == 4.0F && snapshot.lastDecayGameDays == 5.0));
  REQUIRE(store.Size() == 0); // querying never creates a memory

  REQUIRE(store.Register(0, 10.0) == MemoryStatus::InvalidId);
  REQUIRE(store.Get(0, 10.0, snapshot) == MemoryStatus::InvalidId);
  REQUIRE(store.Register(123, -1.0) == MemoryStatus::InvalidArgument);
  REQUIRE(store.Register(123, std::nan("")) == MemoryStatus::InvalidArgument);
  REQUIRE(store.Register(123, 10.0) == MemoryStatus::Created);
  REQUIRE(store.Register(123, 10.0) == MemoryStatus::Ok);
  REQUIRE(store.Size() == 1);

  // A fresh memory is all zeros, stamped with the game time it was made at.
  REQUIRE(store.Get(123, 10.0, snapshot) == MemoryStatus::Ok);
  for (const auto axis : snapshot.axes) {
    REQUIRE(axis == 0.0F);
  }
  REQUIRE(snapshot.lastDecayGameDays == 10.0);

  // Actors are isolated from each other.
  MemoryDelta delta{};
  Slot(delta, Axis::Suspicion) = 35.0F;
  REQUIRE(store.ApplyDelta(123, 10.0, delta, snapshot) == MemoryStatus::Ok);
  REQUIRE(store.Register(456, 10.0) == MemoryStatus::Created);
  REQUIRE(store.Get(456, 10.0, snapshot) == MemoryStatus::Ok);
  REQUIRE(Level(snapshot, Axis::Suspicion) == 0.0F);
  REQUIRE(Current(store, 123, Axis::Suspicion, 10.0) == 35.0F);

  store.Clear();
  REQUIRE(store.Size() == 0);
  REQUIRE(store.Get(123, 10.0, snapshot) == MemoryStatus::NotFound);
}

TEST_CASE("DeltasClampAndAxesAreIndependent") {
  MemoryStore store;
  MemorySnapshot snapshot;
  MemoryDelta delta{};
  Slot(delta, Axis::Fear) = 30.0F;
  Slot(delta, Axis::Respect) = 50.0F;
  REQUIRE(store.ApplyDelta(123, 10.0, delta, snapshot) == MemoryStatus::Ok);
  REQUIRE((Level(snapshot, Axis::Fear) == 30.0F && Level(snapshot, Axis::Respect) == 50.0F));
  // An axis nobody touched stays at zero, and so does a reserve slot.
  REQUIRE((Level(snapshot, Axis::Approval) == 0.0F && Level(snapshot, Axis::Suspicion) == 0.0F));
  REQUIRE(snapshot.axes[axisCapacity - 1] == 0.0F);

  // A follower axis works through the same call even though nothing uses it yet.
  MemoryDelta follower{};
  Slot(follower, Axis::Approval) = 12.0F;
  REQUIRE(store.ApplyDelta(123, 10.0, follower, snapshot) == MemoryStatus::Ok);
  REQUIRE((Level(snapshot, Axis::Approval) == 12.0F && Level(snapshot, Axis::Fear) == 30.0F));
  Slot(follower, Axis::Approval) = -12.0F;
  REQUIRE(store.ApplyDelta(123, 10.0, follower, snapshot) == MemoryStatus::Ok);
  REQUIRE(Level(snapshot, Axis::Approval) == 0.0F);

  // Every axis clamps on its own, at both ends.
  MemoryDelta extremes{};
  Slot(extremes, Axis::Fear) = 200.0F;
  Slot(extremes, Axis::Loyalty) = -5.0F;
  Slot(extremes, Axis::Trust) = 30.0F;
  REQUIRE(store.ApplyDelta(123, 10.0, extremes, snapshot) == MemoryStatus::Ok);
  REQUIRE((Level(snapshot, Axis::Fear) == 100.0F && Level(snapshot, Axis::Loyalty) == 0.0F && Level(snapshot, Axis::Trust) == 30.0F));
  REQUIRE(Level(snapshot, Axis::Respect) == 50.0F);

  // Invalid arguments are refused without touching the output or the memory.
  MemoryDelta poisoned{};
  Slot(poisoned, Axis::Fear) = std::nanf("");
  const auto before = snapshot;
  REQUIRE(store.ApplyDelta(123, 10.0, poisoned, snapshot) == MemoryStatus::InvalidArgument);
  REQUIRE(store.ApplyDelta(123, -1.0, delta, snapshot) == MemoryStatus::InvalidArgument);
  REQUIRE(store.ApplyDelta(0, 10.0, delta, snapshot) == MemoryStatus::InvalidId);
  REQUIRE(Level(snapshot, Axis::Fear) == Level(before, Axis::Fear));
  REQUIRE(store.Size() == 1);
}

TEST_CASE("NamedAxesFitTheReserve") {
  REQUIRE(namedAxes.size() <= axisCapacity);
  for (std::size_t slot = 0; slot < namedAxes.size(); ++slot) {
    REQUIRE(AxisSlot(namedAxes[slot]) == slot);
    REQUIRE(std::strlen(AxisName(namedAxes[slot])) > 0);
  }
  // These numbers are persisted: moving one reinterprets every saved memory.
  REQUIRE((AxisSlot(Axis::Recognition) == 0 && AxisSlot(Axis::Suspicion) == 1 && AxisSlot(Axis::Fear) == 2 && AxisSlot(Axis::Respect) == 3));
  REQUIRE((AxisSlot(Axis::Approval) == 4 && AxisSlot(Axis::Loyalty) == 5 && AxisSlot(Axis::Resentment) == 6 && AxisSlot(Axis::Trust) == 7));
}

TEST_CASE("DecayUsesHalfLives") {
  // Fear fades with a 72-hour half-life: three game days halve it.
  MemoryStore store;
  MemoryDelta delta{};
  Slot(delta, Axis::Fear) = 80.0F;
  MemorySnapshot snapshot;
  REQUIRE(store.ApplyDelta(123, 100.0, delta, snapshot) == MemoryStatus::Ok);
  REQUIRE(store.Get(123, 103.0, snapshot) == MemoryStatus::Ok);
  REQUIRE(Near(Level(snapshot, Axis::Fear), 40.0F));
  REQUIRE(snapshot.lastDecayGameDays == 103.0);

  // Time running backwards neither decays nor rewinds the stamp.
  REQUIRE(store.Get(123, 90.0, snapshot) == MemoryStatus::Ok);
  REQUIRE(Near(Level(snapshot, Axis::Fear), 40.0F));
  REQUIRE(snapshot.lastDecayGameDays == 103.0);

  // Registering an existing memory brings it forward without changing it.
  REQUIRE(store.Register(123, 106.0) == MemoryStatus::Ok);
  REQUIRE(Near(Current(store, 123, Axis::Fear, 106.0), 20.0F));

  // A reserve slot with no half-life holds its value while tuned axes fade.
  MemoryStore fresh;
  MemoryDelta mixed{};
  Slot(mixed, Axis::Approval) = 60.0F;
  Slot(mixed, Axis::Fear) = 80.0F;
  REQUIRE(fresh.ApplyDelta(1, 1.0, mixed, snapshot) == MemoryStatus::Ok);
  REQUIRE(fresh.Get(1, 4.0, snapshot) == MemoryStatus::Ok);
  REQUIRE(Level(snapshot, Axis::Approval) == 60.0F);
  REQUIRE(Level(snapshot, Axis::Fear) < 80.0F);

  // The store decays by the tuning it was built with, not by the defaults.
  Tuning quick;
  quick.decay.hours[AxisSlot(Axis::Fear)] = 24.0;
  MemoryStore custom(quick);
  MemoryDelta fear{};
  Slot(fear, Axis::Fear) = 80.0F;
  REQUIRE(custom.ApplyDelta(1, 1.0, fear, snapshot) == MemoryStatus::Ok);
  REQUIRE(custom.Get(1, 2.0, snapshot) == MemoryStatus::Ok);
  REQUIRE(Near(Level(snapshot, Axis::Fear), 40.0F));
}

TEST_CASE("ThresholdAndWeights") {
  const RecognitionTuning recognition;
  REQUIRE(Near(recognition.Threshold(0.0F), 0.70F));
  REQUIRE(Near(recognition.Threshold(100.0F), 0.50F));
  REQUIRE(Near(recognition.Threshold(50.0F), 0.60F));
  // Out-of-range recognition stays inside the band.
  REQUIRE(Near(recognition.Threshold(-50.0F), 0.70F));
  REQUIRE(Near(recognition.Threshold(500.0F), 0.50F));

  const AppearanceWeights weights;
  REQUIRE(weights.Get(AppearanceCategory::Head) == weights.head);
  REQUIRE(weights.Get(AppearanceCategory::Body) == weights.body);
  REQUIRE(weights.Get(AppearanceCategory::Hands) == weights.hands);
  REQUIRE(weights.Get(AppearanceCategory::Feet) == weights.feet);
  REQUIRE(weights.Get(AppearanceCategory::Shield) == weights.shield);
  REQUIRE(weights.Get(AppearanceCategory::Accessories) == weights.accessories);
}

TEST_SUITE_END();
