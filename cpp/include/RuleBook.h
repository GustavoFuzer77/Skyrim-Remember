#pragma once

#include "MemoryTuning.h"
#include "NpcProfile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Impressions: what the Player's equipment makes an NPC feel, declared as data.
//
// Three decisions shape this:
//
// - Rules match keywords first. ArmorMaterialHide covers every hide armor of
//   every mod. A specific Form is the fallback for items that carry no
//   distinguishing keyword, like vanilla bandit armor.
// - Equipment is a state, not an event. Wearing something implies a level an
//   axis is raised to, never a delta added on every scan: the host polls, and an
//   additive rule would saturate every axis within seconds.
// - Rules project, temperaments convert. A rule says what an item suggests
//   (banditry, threat); each profile's temperament turns that into its own axes.
//   A new rule reaches every profile, and a new profile reuses every rule.
//
// Nothing here knows about Skyrim. The host resolves every RuleReference to a
// runtime FormID and passes worn Forms and keywords as plain integers.
namespace SkyrimRemember {

enum class RuleReferenceKind : std::uint32_t { Keyword = 0, Form = 1 };

// Something a rule matches that only the host can turn into a runtime FormID.
struct RuleReference {
  RuleReferenceKind kind = RuleReferenceKind::Keyword;
  // A keyword's EditorID, or the file name of the plugin that defines a Form.
  std::string name;
  // The Form's ID local to its plugin; zero for a keyword.
  std::uint32_t localFormId = 0;

  auto operator<=>(const RuleReference &) const = default;
};

// The axis levels one outfit implies for one profile.
struct ImpressionOutcome {
  std::array<float, axisCapacity> targets{};
  std::uint32_t matchedRules = 0;
};

// Every loaded rule, and which runtime FormIDs trigger them.
class RuleBook {
public:
  // Adds one rule file. The whole file is validated first, so a rejected file
  // changes nothing and the reason, phrased for the person who wrote it, is
  // returned. Impressions accumulate across files; a later file's temperament
  // entry replaces an earlier one for the same profile and projection, which is
  // how a mod retunes the defaults.
  std::optional<std::string> LoadJson(std::string_view json);

  std::size_t ImpressionCount() const {
    return impressions.size();
  }

  // Every distinct keyword and Form the loaded rules mention, in load order.
  const std::vector<RuleReference> &References() const {
    return references;
  }

  std::size_t BoundCount() const;

  // Records the runtime FormID the host resolved a reference to. Zero marks it
  // unresolved. Returns false for an index no rule produced.
  bool Bind(std::size_t reference, std::uint32_t runtimeFormId);

  // The axis levels this outfit implies for a profile.
  //
  // Within one projection the strongest matching rule wins, and across
  // projections each axis takes the highest converted level. Nothing adds up, so
  // wearing three bandit pieces is no more suspicious than the most suspicious
  // one, and the result is the same however often it is asked.
  ImpressionOutcome Evaluate(NpcProfile profile, std::span<const std::uint32_t> worn) const;

private:
  struct Impression {
    // Projection index and strength.
    std::vector<std::pair<std::size_t, float>> projections;
  };

  std::size_t InternProjection(const std::string &name);
  std::size_t InternReference(const RuleReference &reference);
  void RebuildBound();

  std::vector<Impression> impressions;
  std::vector<RuleReference> references;
  std::map<RuleReference, std::size_t> referenceIndex;
  // Impressions each reference triggers.
  std::vector<std::vector<std::size_t>> triggers;
  // Runtime FormID of each reference, once the host resolved it.
  std::vector<std::optional<std::uint32_t>> bindings;
  // Runtime FormID to the impressions it triggers. Keywords and Forms share one
  // FormID space, so one map serves both.
  std::unordered_map<std::uint32_t, std::vector<std::size_t>> bound;
  std::vector<std::string> projections;
  std::unordered_map<std::string, std::size_t> projectionIndex;
  // Per profile: projection index to the axis weights it converts into.
  std::array<std::unordered_map<std::size_t, std::vector<std::pair<Axis, float>>>, npcProfileCount> temperaments;
};

} // namespace SkyrimRemember
