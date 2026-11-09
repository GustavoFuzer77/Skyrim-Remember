#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// Every number the recognition model can be tuned by, in one place.
//
// The algorithms in MemoryStore read these values from the Tuning it carries
// instead of embedding literals, so a future configuration file has a single
// structure to fill in.
namespace SkyrimRemember {

// How many axis slots every memory carries. Deliberately larger than the number
// of axes named below: the spare slots are what let a new axis be named without
// resizing any struct or bumping the persisted payload. An older save loads an
// axis it never knew at zero, which is the correct value.
inline constexpr std::size_t axisCapacity = 16;

// One dimension of what an NPC feels about the Player.
//
// Storage is uniform: every NPC carries every slot. Which axes actually get
// written is a matter of behaviour — a guard's temperament touches fear and
// respect, a follower's would touch approval and loyalty — never of type. That
// is what lets an NPC change profile without converting or losing anything.
//
// The slot numbers are persisted: reordering these would silently reinterpret
// every saved memory.
enum class Axis : std::uint32_t {
  // How well the NPC knows the Player by sight. Read by the appearance system,
  // so it is the one axis every profile shares.
  Recognition = 0,
  Suspicion = 1,
  Fear = 2,
  Respect = 3,
  // Reserved for the Follower profile; nothing writes these yet.
  Approval = 4,
  Loyalty = 5,
  Resentment = 6,
  Trust = 7
};

constexpr std::size_t AxisSlot(Axis axis) {
  return static_cast<std::size_t>(axis);
}

// Every named axis, in slot order. The remaining slots up to axisCapacity are
// reserve and always read as zero.
inline constexpr std::array namedAxes = {Axis::Recognition, Axis::Suspicion, Axis::Fear, Axis::Respect, Axis::Approval, Axis::Loyalty, Axis::Resentment, Axis::Trust};

static_assert(AxisSlot(namedAxes.back()) < axisCapacity);

// The name rule files and log lines use.
constexpr const char *AxisName(Axis axis) {
  switch (axis) {
  case Axis::Recognition:
    return "recognition";
  case Axis::Suspicion:
    return "suspicion";
  case Axis::Fear:
    return "fear";
  case Axis::Respect:
    return "respect";
  case Axis::Approval:
    return "approval";
  case Axis::Loyalty:
    return "loyalty";
  case Axis::Resentment:
    return "resentment";
  case Axis::Trust:
    return "trust";
  }
  return "unknown";
}

// Upper bound of every memory axis. Also the divisor that turns a raw
// recognition value into the 0..=1 familiarity ratio.
inline constexpr float maxMemoryValue = 100.0F;

// Hard cap on one appearance observation. A safety limit rather than a tunable:
// Limits::maxAppearanceItems may only lower it.
inline constexpr std::size_t maxAppearanceItems = 64;

// Default number of distinct outfits one NPC remembers.
inline constexpr std::size_t maxKnownAppearances = 8;

// Hard cap on the keywords passed with one impression.
inline constexpr std::size_t maxWornKeywords = 512;

// Largest rule file the host may hand over. Rule files are small; anything near
// this is a mistake, not a configuration.
inline constexpr std::size_t maxRuleFileBytes = 1024 * 1024;

// The smallest rise worth reporting. A worn item keeps topping its axis back up
// as memory decays between scans, and those tiny corrections are not news.
inline constexpr float noticeableRise = 1.0F;

// A group of biped slots that reads as one visual feature at a distance.
enum class AppearanceCategory { Head, Body, Hands, Feet, Shield, Accessories };

// Half-life, in game hours, of every memory axis slot.
//
// A slot left at zero does not fade at all. That is the deliberate default for
// the reserve slots: an axis added without tuning keeps its value instead of
// silently draining away.
struct DecayHalfLives {
  std::array<double, axisCapacity> hours = [] {
    std::array<double, axisCapacity> defaults{};
    defaults[AxisSlot(Axis::Recognition)] = 24.0 * 30.0;
    defaults[AxisSlot(Axis::Suspicion)] = 48.0;
    defaults[AxisSlot(Axis::Fear)] = 72.0;
    defaults[AxisSlot(Axis::Respect)] = 24.0 * 30.0;
    return defaults;
  }();
};

// How much outfit evidence an NPC needs before it accepts an identity.
//
// A stranger has to match baseThreshold; a fully familiar NPC only has to match
// baseThreshold - familiarityBonus, floored at minimumThreshold.
struct RecognitionTuning {
  float baseThreshold = 0.70F;
  float familiarityBonus = 0.20F;
  float minimumThreshold = 0.50F;

  // Similarity an outfit must reach for an NPC with this recognition.
  float Threshold(float recognition) const {
    const auto familiarity = std::clamp(recognition / maxMemoryValue, 0.0F, 1.0F);
    return std::clamp(baseThreshold - familiarity * familiarityBonus, minimumThreshold, baseThreshold);
  }
};

// Relative importance of each equipment category when comparing two outfits.
//
// Only the categories present in at least one of the two outfits count, so the
// weights are renormalized per comparison and need not sum to one.
struct AppearanceWeights {
  float head = 0.25F;
  float body = 0.35F;
  float hands = 0.10F;
  float feet = 0.10F;
  float shield = 0.15F;
  float accessories = 0.05F;

  float Get(AppearanceCategory category) const {
    switch (category) {
    case AppearanceCategory::Head:
      return head;
    case AppearanceCategory::Body:
      return body;
    case AppearanceCategory::Hands:
      return hands;
    case AppearanceCategory::Feet:
      return feet;
    case AppearanceCategory::Shield:
      return shield;
    case AppearanceCategory::Accessories:
      return accessories;
    }
    return 0.0F;
  }
};

// How an observation that carries nothing new is allowed to touch memory.
struct MemoryObservationTuning {
  // Minimum game time between two refreshes of the same known outfit.
  //
  // The host polls: a guard standing next to the Player observes the same
  // unchanged outfit over and over. Without this floor, that outfit's
  // lastSeenGameDays would be rewritten on every tick, it would never be the
  // eviction candidate, and freshness would measure how long the scan ran
  // rather than when the NPC last saw something.
  double refreshIntervalGameDays = 1.0 / 24.0; // one game hour

  // Familiarity earned when the NPC gets a qualifying view of the Player's
  // face. Covered faces and visual continuity do not teach facial identity.
  float recognitionGain = 5.0F;

  // Minimum game time between familiarity gains for one NPC. Deliberately
  // longer than the scan cadence so polling cannot farm it: six game hours
  // means twenty spaced encounters reach full familiarity.
  double recognitionIntervalGameDays = 6.0 / 24.0;
};

// Storage bounds enforced by the store itself.
struct Limits {
  std::size_t maxKnownAppearances = SkyrimRemember::maxKnownAppearances;
  std::size_t maxAppearanceItems = SkyrimRemember::maxAppearanceItems;
};

// The complete parameter set of one memory store.
struct Tuning {
  DecayHalfLives decay;
  RecognitionTuning recognition;
  AppearanceWeights weights;
  Limits limits;
  MemoryObservationTuning observation;
};

} // namespace SkyrimRemember
