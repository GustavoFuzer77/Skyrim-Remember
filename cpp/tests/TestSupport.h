#pragma once

#include "Appearance.h"
#include "Memory.h"
#include "RuleBook.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Helpers shared by every test file. Everything under test runs without Skyrim:
// the memory model, recognition, rules and persistence are plain C++, and the
// host-side policy that does not read the game (outfit merging, profiles, guard
// reactions, notices) is exercised too.
namespace SkyrimRemember::Testing {

namespace Biped = SkyrimRemember::BipedSlot;

inline constexpr std::uint32_t bodySlot = 1U << 2;
inline constexpr std::uint32_t handsSlot = 1U << 3;

inline bool Near(float left, float right, float tolerance = 0.0001F) {
  return std::fabs(left - right) < tolerance;
}

inline float Level(const MemorySnapshot &snapshot, Axis axis) {
  return snapshot.axes[AxisSlot(axis)];
}

inline float &Slot(MemoryDelta &delta, Axis axis) {
  return delta.axes[AxisSlot(axis)];
}

inline AppearanceItem Item(std::uint32_t formId, std::uint32_t slotMask = bodySlot) {
  return {formId, slotMask};
}

// One memory at a game time, the way the host registers an observer.
inline void Know(MemoryStore &store, std::uint32_t id, double gameDays = 1.0) {
  store.Register(id, gameDays);
}

// The helpers below throw instead of returning a status, so a test that misuses
// them fails with a message instead of carrying on with garbage.
inline AppearanceResult Observe(MemoryStore &store, std::uint32_t id, std::vector<AppearanceItem> items, bool face, bool continuity, double gameDays) {
  AppearanceResult result;
  if (store.ObserveAppearance(id, gameDays, items, face, continuity, result) != MemoryStatus::Ok) {
    throw std::logic_error("ObserveAppearance did not return Ok");
  }
  return result;
}

inline float Current(MemoryStore &store, std::uint32_t id, Axis axis, double gameDays = 1.0) {
  MemorySnapshot snapshot;
  if (store.Get(id, gameDays, snapshot) != MemoryStatus::Ok) {
    throw std::logic_error("Get did not return Ok");
  }
  return Level(snapshot, axis);
}

// The stored value, without bringing the memory forward in time first.
inline float Raw(MemoryStore &store, std::uint32_t id, Axis axis) {
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

inline std::vector<std::uint8_t> FromHex(std::string_view hex) {
  std::vector<std::uint8_t> bytes;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    bytes.push_back(static_cast<std::uint8_t>(std::stoi(std::string{hex.substr(index, 2)}, nullptr, 16)));
  }
  return bytes;
}

inline const FormIdResolver keepIds = [](std::uint32_t id) { return std::optional{id}; };

// ---------------------------------------------------------------------------
// Rule fixtures, shared by the rule book and the store's impression tests.

inline constexpr std::uint32_t banditCuirass = 0x0006F393;
inline constexpr std::uint32_t hideKeyword = 0x01000001;
inline constexpr std::uint32_t ironKeyword = 0x01000002;

inline constexpr std::string_view banditRules = R"({
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
inline void BindLikeAHost(RuleBook &rules) {
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

inline bool Loaded(RuleBook &rules, std::string_view json) {
  return !rules.LoadJson(json).has_value();
}

inline float Target(const ImpressionOutcome &outcome, Axis axis) {
  return outcome.targets[AxisSlot(axis)];
}

} // namespace SkyrimRemember::Testing
