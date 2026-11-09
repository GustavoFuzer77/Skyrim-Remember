#pragma once

#include "MemoryTuning.h"
#include "NpcProfile.h"
#include "RuleBook.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Who an NPC remembers and how strongly, and how well it recognizes the Player
// by what the Player wears.
//
// Nothing in here knows about Skyrim: the host passes FormIDs as plain integers
// and resolves them itself. That is what lets the whole algorithm — decay,
// outfit comparison, recognition, impressions, persistence — run in a plain test
// executable without the game installed.
namespace SkyrimRemember {

enum class MemoryStatus : std::uint32_t {
  Ok = 0,
  Created = 1,
  InvalidId = 2,
  NotFound = 3,
  InvalidArgument = 4,
  InvalidData = 5,
  UnsupportedVersion = 6
};

// Indexed with AxisSlot(Axis::...). Slots past the named axes read as zero.
struct MemorySnapshot {
  std::array<float, axisCapacity> axes{};
  double lastDecayGameDays = 0.0;
};

// One change per axis slot. A slot left at zero leaves that axis alone.
struct MemoryDelta {
  std::array<float, axisCapacity> axes{};
};

struct PersistenceStats {
  std::uint32_t loaded = 0;
  // Number of unresolved NPC references and equipped item Forms.
  std::uint32_t unresolved = 0;
};

// One worn Form and the biped slots it occupies.
struct AppearanceItem {
  std::uint32_t formId = 0;
  std::uint32_t slotMask = 0;

  bool operator==(const AppearanceItem &) const = default;
};

struct AppearanceResult {
  float outfitSimilarity = 0.0F;
  float identityConfidence = 0.0F;
  float recognitionThreshold = 0.0F;
  bool appearanceMatched = false;
  bool recognized = false;
  bool linkedByContinuity = false;
  bool learned = false;
  bool faceVisible = false;
  std::uint32_t itemCount = 0;
  std::uint32_t knownAppearances = 0;
};

// What one outfit did to one NPC's axes.
struct ImpressionResult {
  // The level each slot was raised to; indexed with AxisSlot(Axis::...).
  std::array<float, axisCapacity> targets{};
  // One bit per slot that rose noticeably: 1 << AxisSlot(axis).
  std::uint32_t raisedAxes = 0;
  std::uint32_t matchedRules = 0;
};

struct RulesSummary {
  std::uint32_t impressions = 0;
  std::uint32_t references = 0;
  std::uint32_t boundReferences = 0;
};

// Maps a FormID read from a save to the one it has in the running game. Empty
// when the Form no longer exists.
using FormIdResolver = std::function<std::optional<std::uint32_t>(std::uint32_t)>;

// What one NPC feels about the Player, and when that was last brought up to date.
struct NpcMemory {
  std::array<float, axisCapacity> axes{};
  double lastDecayGameDays = 0.0;
  std::optional<double> lastRecognitionGameDays;

  bool IsValid() const;
};

// An outfit one NPC has already seen, and whether it knows whose it is.
struct KnownAppearance {
  std::vector<AppearanceItem> items;
  bool faceVisible = false;
  bool identityKnown = false;
  double lastSeenGameDays = 0.0;
};

// Strips technical slots, drops empty entries, sorts by FormID, and merges the
// slots of one Form worn in several slots, so that two equal outfits compare
// bit-for-bit equal.
void NormalizeItems(std::vector<AppearanceItem> &items);

// Weighted Jaccard similarity of two outfits, in 0..=1.
//
// Categories absent from both outfits are excluded and their weight is not
// counted, so comparing two unarmoured Players is not penalised for the helmet
// that neither of them wears.
float OutfitSimilarity(std::span<const AppearanceItem> left, std::span<const AppearanceItem> right, const AppearanceWeights &weights);

// Every NPC memory of one session, plus the parameters they are evaluated by.
//
// Every public call takes the lock, so the store may be used from any thread.
// Persisted data and rules are separate lifetimes: Clear and Deserialize replace
// memories and leave the loaded rules alone.
class MemoryStore {
public:
  explicit MemoryStore(Tuning tuning = {});

  const Tuning &GetTuning() const {
    return tuning;
  }

  // Registers a reference without changing existing values. Created when the
  // memory had to be made, Ok when it already existed.
  MemoryStatus Register(std::uint32_t referenceFormId, double gameDays);

  // Reads a snapshot, bringing the memory forward in time. Never creates one;
  // on an error the output is left untouched.
  MemoryStatus Get(std::uint32_t referenceFormId, double gameDays, MemorySnapshot &output);

  // Applies a gameplay delta after bringing the memory forward to gameDays,
  // creating it when absent. Every axis is clamped to 0..=maxMemoryValue.
  MemoryStatus ApplyDelta(std::uint32_t referenceFormId, double gameDays, const MemoryDelta &delta, MemorySnapshot &output);

  // Compares the Player's current equipment with the appearances one NPC knows,
  // and learns from it. The first outfit an NPC sees is always learned; later
  // novel outfits only when the NPC can attach them to a known identity, through
  // a visible face, a matching outfit, or host-supplied visual continuity.
  MemoryStatus ObserveAppearance(std::uint32_t referenceFormId, double gameDays, std::span<const AppearanceItem> items, bool faceVisible, bool visualContinuity,
                                 AppearanceResult &output);

  // The same verdict without learning, refreshing or creating anything.
  MemoryStatus EvaluateAppearance(std::uint32_t referenceFormId, std::span<const AppearanceItem> items, bool faceVisible, bool visualContinuity,
                                  AppearanceResult &output);

  // Raises one NPC's axes to what the worn items and keywords imply for its
  // profile. Never lowers anything. Call only for an NPC that recognized the
  // Player: the outfit of a stranger it could not identify is not held against
  // the Player.
  MemoryStatus ObserveImpression(std::uint32_t referenceFormId, double gameDays, NpcProfile profile, std::span<const AppearanceItem> items,
                                 std::span<const std::uint32_t> keywords, ImpressionResult &output);

  // Adds one JSON rule file. Returns the reason when the file was rejected, and
  // then changes nothing.
  std::optional<std::string> LoadRules(std::string_view json);
  RulesSummary GetRulesSummary() const;
  std::optional<RuleReference> GetRuleReference(std::uint32_t index) const;
  // Records the runtime FormID a reference resolved to; zero marks it
  // unresolved. NotFound for an index no rule produced.
  MemoryStatus BindRuleReference(std::uint32_t index, std::uint32_t runtimeFormId);

  // The complete store as one payload for the SKSE co-save.
  std::vector<std::uint8_t> Serialize() const;

  // Replaces the memories from a payload, resolving every saved FormID through
  // resolver first. Unresolved entries are skipped and counted; malformed input
  // leaves the previous memories untouched.
  MemoryStatus Deserialize(std::span<const std::uint8_t> bytes, const FormIdResolver &resolver, PersistenceStats &stats);

  // New game, revert, or before loading persisted data.
  void Clear();

  // Test and diagnostic access.
  std::size_t Size() const;
  std::optional<NpcMemory> Find(std::uint32_t referenceFormId) const;
  std::vector<KnownAppearance> KnownAppearances(std::uint32_t referenceFormId) const;

private:
  NpcMemory *Touch(std::uint32_t referenceFormId, double gameDays);
  NpcMemory &TouchOrCreate(std::uint32_t referenceFormId, double gameDays);
  std::optional<AppearanceResult> EvaluateNormalized(std::uint32_t referenceFormId, std::span<const AppearanceItem> items, bool faceVisible,
                                                     bool visualContinuity) const;

  mutable std::mutex mutex;
  const Tuning tuning;
  std::unordered_map<std::uint32_t, NpcMemory> memories;
  std::unordered_map<std::uint32_t, std::vector<KnownAppearance>> appearances;
  // Loaded once from the game's rule files. Belongs to the session, not to a
  // save: clearing or restoring memories leaves it alone.
  RuleBook rules;
};

} // namespace SkyrimRemember
