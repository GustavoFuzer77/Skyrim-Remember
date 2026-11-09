#include "RuleBook.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <stdexcept>

namespace SkyrimRemember {
namespace {

using Json = nlohmann::json;

// Thrown while validating a file; never escapes LoadJson.
struct Rejection : std::runtime_error {
  using std::runtime_error::runtime_error;
};

constexpr std::array<const char *, npcProfileCount> profileNames = {"guard", "follower", "citizen", "merchant", "generic"};

std::optional<NpcProfile> ProfileFromName(std::string_view name) {
  for (std::size_t index = 0; index < profileNames.size(); ++index) {
    if (name == profileNames[index]) {
      return static_cast<NpcProfile>(index);
    }
  }
  return std::nullopt;
}

std::optional<Axis> AxisFromName(std::string_view name) {
  const auto found = std::ranges::find_if(namedAxes, [name](Axis axis) { return name == AxisName(axis); });
  return found == namedAxes.end() ? std::nullopt : std::optional{*found};
}

std::string_view Trim(std::string_view text) {
  constexpr std::string_view whitespace = " \t\r\n";
  const auto first = text.find_first_not_of(whitespace);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

// Parses 0x06F393~Skyrim.esm, the notation SPID and KID users already know.
RuleReference ParseForm(const std::string &text) {
  const auto separator = text.find('~');
  if (separator == std::string::npos) {
    throw Rejection(std::format("form \"{}\" must look like 0x06F393~Skyrim.esm", text));
  }
  auto id = Trim(std::string_view{text}.substr(0, separator));
  const auto plugin = Trim(std::string_view{text}.substr(separator + 1));
  if (id.starts_with("0x") || id.starts_with("0X")) {
    id.remove_prefix(2);
  }
  std::uint32_t localId = 0;
  const auto *end = id.data() + id.size();
  const auto parsed = std::from_chars(id.data(), end, localId, 16);
  if (id.empty() || parsed.ec != std::errc{} || parsed.ptr != end) {
    throw Rejection(std::format("form \"{}\" has an invalid hexadecimal id", text));
  }
  if (localId == 0 || localId > 0x00FFFFFFU) {
    throw Rejection(std::format("form \"{}\" id must be a local id between 0x1 and 0xFFFFFF", text));
  }
  if (plugin.empty()) {
    throw Rejection(std::format("form \"{}\" names no plugin", text));
  }
  return {RuleReferenceKind::Form, std::string{plugin}, localId};
}

const Json &Member(const Json &object, const char *key, const std::string &where, bool required) {
  static const Json absent;
  const auto found = object.find(key);
  if (found == object.end()) {
    if (required) {
      throw Rejection(std::format("{}: missing field \"{}\"", where, key));
    }
    return absent;
  }
  return *found;
}

void RejectUnknownFields(const Json &object, std::initializer_list<const char *> known, const std::string &where) {
  for (const auto &[key, value] : object.items()) {
    if (std::ranges::none_of(known, [&key](const char *name) { return key == name; })) {
      throw Rejection(std::format("{}: unknown field \"{}\"", where, key));
    }
  }
}

const Json &ExpectObject(const Json &value, const std::string &where) {
  if (!value.is_object()) {
    throw Rejection(std::format("{}: expected an object", where));
  }
  return value;
}

const Json &ExpectArray(const Json &value, const std::string &where) {
  if (!value.is_array()) {
    throw Rejection(std::format("{}: expected an array", where));
  }
  return value;
}

float ExpectNumber(const Json &value, const std::string &where) {
  if (!value.is_number()) {
    throw Rejection(std::format("{}: expected a number", where));
  }
  return static_cast<float>(value.get<double>());
}

std::vector<std::string> ExpectStrings(const Json &value, const std::string &where) {
  std::vector<std::string> strings;
  for (const auto &element : ExpectArray(value, where)) {
    if (!element.is_string()) {
      throw Rejection(std::format("{}: expected a string", where));
    }
    strings.push_back(element.get<std::string>());
  }
  return strings;
}

// A list that may be left out of the file, or written as null.
std::vector<std::string> OptionalStrings(const Json &object, const char *key, const std::string &label) {
  const auto found = object.find(key);
  if (found == object.end() || found->is_null()) {
    return {};
  }
  return ExpectStrings(*found, std::format("{}.{}", label, key));
}

bool ValidStrength(float value) {
  return std::isfinite(value) && value >= 0.0F && value <= maxMemoryValue;
}

struct StagedImpression {
  std::vector<RuleReference> references;
  std::vector<std::pair<std::string, float>> projections;
};

struct StagedTemperament {
  NpcProfile profile;
  std::string projection;
  std::vector<std::pair<Axis, float>> weights;
};

StagedImpression StageImpression(const Json &entry, std::size_t position) {
  const auto base = std::format("impressions[{}]", position);
  ExpectObject(entry, base);
  RejectUnknownFields(entry, {"name", "keywords", "forms", "projects"}, base);

  const auto &name = Member(entry, "name", base, false);
  if (!name.is_null() && !name.is_string()) {
    throw Rejection(std::format("{}: \"name\" must be a string", base));
  }
  const auto label = name.is_string() ? name.get<std::string>() : base;

  const auto keywords = OptionalStrings(entry, "keywords", label);
  const auto forms = OptionalStrings(entry, "forms", label);
  if (keywords.empty() && forms.empty()) {
    throw Rejection(std::format("{}: needs at least one keyword or form", label));
  }
  const auto &projects = ExpectObject(Member(entry, "projects", label, true), label + ".projects");
  if (projects.empty()) {
    throw Rejection(std::format("{}: \"projects\" needs at least one projection", label));
  }

  StagedImpression staged;
  for (const auto &keyword : keywords) {
    const auto trimmed = Trim(keyword);
    if (trimmed.empty()) {
      throw Rejection(std::format("{}: empty keyword", label));
    }
    staged.references.push_back({RuleReferenceKind::Keyword, std::string{trimmed}, 0});
  }
  for (const auto &form : forms) {
    try {
      staged.references.push_back(ParseForm(form));
    } catch (const Rejection &rejection) {
      throw Rejection(std::format("{}: {}", label, rejection.what()));
    }
  }
  for (const auto &[projection, value] : projects.items()) {
    if (Trim(projection).empty()) {
      throw Rejection(std::format("{}: empty projection name", label));
    }
    const auto strength = ExpectNumber(value, std::format("{}.projects.{}", label, projection));
    if (!ValidStrength(strength)) {
      throw Rejection(std::format("{}: projection \"{}\" strength {} must be between 0 and {}", label, projection, strength, maxMemoryValue));
    }
    staged.projections.emplace_back(projection, strength);
  }
  return staged;
}

void StageTemperaments(const Json &section, std::vector<StagedTemperament> &staged) {
  for (const auto &[profileName, projections] : ExpectObject(section, "temperaments").items()) {
    const auto profile = ProfileFromName(profileName);
    if (!profile) {
      throw Rejection(std::format("temperaments: unknown profile \"{}\", expected guard, follower, citizen, merchant or generic", profileName));
    }
    for (const auto &[projection, axes] : ExpectObject(projections, "temperaments." + profileName).items()) {
      const auto where = std::format("temperaments.{}.{}", profileName, projection);
      std::vector<std::pair<Axis, float>> weights;
      for (const auto &[axisName, value] : ExpectObject(axes, where).items()) {
        const auto axis = AxisFromName(axisName);
        if (!axis) {
          throw Rejection(std::format("{}: unknown axis \"{}\"", where, axisName));
        }
        const auto weight = ExpectNumber(value, where + "." + axisName);
        if (!std::isfinite(weight) || weight < 0.0F) {
          throw Rejection(std::format("{}.{}: weight {} must be zero or positive", where, axisName, weight));
        }
        weights.emplace_back(*axis, weight);
      }
      staged.push_back({*profile, projection, std::move(weights)});
    }
  }
}

} // namespace

std::optional<std::string> RuleBook::LoadJson(std::string_view json) {
  std::vector<StagedImpression> stagedImpressions;
  std::vector<StagedTemperament> stagedTemperaments;
  try {
    const auto file = Json::parse(json);
    ExpectObject(file, "rule file");
    RejectUnknownFields(file, {"impressions", "temperaments"}, "rule file");

    if (const auto found = file.find("impressions"); found != file.end() && !found->is_null()) {
      std::size_t position = 0;
      for (const auto &entry : ExpectArray(*found, "impressions")) {
        stagedImpressions.push_back(StageImpression(entry, position++));
      }
    }
    if (const auto found = file.find("temperaments"); found != file.end() && !found->is_null()) {
      StageTemperaments(*found, stagedTemperaments);
    }
  } catch (const Rejection &rejection) {
    return rejection.what();
  } catch (const Json::exception &failure) {
    return failure.what();
  }

  // Validation is over: nothing below can fail, so a rejected file never leaves
  // half of itself behind.
  for (const auto &staged : stagedImpressions) {
    const auto impression = impressions.size();
    Impression added;
    for (const auto &[name, strength] : staged.projections) {
      added.projections.emplace_back(InternProjection(name), strength);
    }
    impressions.push_back(std::move(added));
    for (const auto &reference : staged.references) {
      auto &impressionsOfReference = triggers[InternReference(reference)];
      if (std::ranges::find(impressionsOfReference, impression) == impressionsOfReference.end()) {
        impressionsOfReference.push_back(impression);
      }
    }
  }
  for (auto &staged : stagedTemperaments) {
    temperaments[static_cast<std::size_t>(staged.profile)][InternProjection(staged.projection)] = std::move(staged.weights);
  }
  RebuildBound();
  return std::nullopt;
}

std::size_t RuleBook::BoundCount() const {
  return static_cast<std::size_t>(std::ranges::count_if(bindings, [](const auto &binding) { return binding.has_value(); }));
}

bool RuleBook::Bind(std::size_t reference, std::uint32_t runtimeFormId) {
  if (reference >= bindings.size()) {
    return false;
  }
  bindings[reference] = runtimeFormId != 0 ? std::optional{runtimeFormId} : std::nullopt;
  RebuildBound();
  return true;
}

ImpressionOutcome RuleBook::Evaluate(NpcProfile profile, std::span<const std::uint32_t> worn) const {
  ImpressionOutcome outcome;
  if (bound.empty()) {
    return outcome;
  }
  std::vector<bool> matched(impressions.size(), false);
  for (const auto formId : worn) {
    if (const auto found = bound.find(formId); found != bound.end()) {
      for (const auto impression : found->second) {
        matched[impression] = true;
      }
    }
  }

  std::vector<float> strengths(projections.size(), 0.0F);
  for (std::size_t impression = 0; impression < matched.size(); ++impression) {
    if (!matched[impression]) {
      continue;
    }
    ++outcome.matchedRules;
    for (const auto &[projection, strength] : impressions[impression].projections) {
      strengths[projection] = std::max(strengths[projection], strength);
    }
  }

  const auto &temperament = temperaments[static_cast<std::size_t>(profile)];
  for (std::size_t projection = 0; projection < strengths.size(); ++projection) {
    if (strengths[projection] <= 0.0F) {
      continue;
    }
    const auto weights = temperament.find(projection);
    if (weights == temperament.end()) {
      continue;
    }
    for (const auto &[axis, weight] : weights->second) {
      const auto level = std::clamp(strengths[projection] * weight, 0.0F, maxMemoryValue);
      auto &target = outcome.targets[AxisSlot(axis)];
      target = std::max(target, level);
    }
  }
  return outcome;
}

std::size_t RuleBook::InternProjection(const std::string &name) {
  if (const auto found = projectionIndex.find(name); found != projectionIndex.end()) {
    return found->second;
  }
  const auto index = projections.size();
  projections.push_back(name);
  projectionIndex.emplace(name, index);
  return index;
}

std::size_t RuleBook::InternReference(const RuleReference &reference) {
  if (const auto found = referenceIndex.find(reference); found != referenceIndex.end()) {
    return found->second;
  }
  const auto index = references.size();
  references.push_back(reference);
  referenceIndex.emplace(reference, index);
  triggers.emplace_back();
  bindings.emplace_back();
  return index;
}

void RuleBook::RebuildBound() {
  bound.clear();
  for (std::size_t reference = 0; reference < bindings.size(); ++reference) {
    if (!bindings[reference]) {
      continue;
    }
    auto &impressionsOfForm = bound[*bindings[reference]];
    for (const auto impression : triggers[reference]) {
      if (std::ranges::find(impressionsOfForm, impression) == impressionsOfForm.end()) {
        impressionsOfForm.push_back(impression);
      }
    }
  }
  std::erase_if(bound, [](const auto &entry) { return entry.second.empty(); });
}

} // namespace SkyrimRemember
