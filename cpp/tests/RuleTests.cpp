// Impression rules: the rule file format, the rule book, and how the store
// turns what the Player wears into memory.

#include "TestSupport.h"
#include "NpcProfile.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace SkyrimRemember;
using namespace SkyrimRemember::Testing;

TEST_SUITE_BEGIN("rules");

TEST_CASE("TheShippedRuleFileIsValid") {
  std::ifstream input(SHIPPED_RULES_PATH, std::ios::binary);
  REQUIRE(input.is_open());
  const std::string json{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};

  RuleBook rules;
  const auto rejection = rules.LoadJson(json);
  INFO("shipped rules rejected: ", rejection.value_or(""));
  REQUIRE(!rejection);
  REQUIRE(rules.ImpressionCount() >= 1);
  const RuleReference bandit{RuleReferenceKind::Form, "Skyrim.esm", banditCuirass};
  REQUIRE(std::ranges::find(rules.References(), bandit) != rules.References().end());
}

TEST_CASE("ReferencesAreListedOnceInLoadOrder") {
  RuleBook rules;
  REQUIRE(Loaded(rules, R"({ "impressions": [
      { "keywords": ["A", "B"], "projects": { "x": 1 } },
      { "keywords": ["B"], "forms": ["800~Mod.esp"], "projects": { "x": 2 } }
    ] })"));

  const std::vector<RuleReference> expected = {
      {RuleReferenceKind::Keyword, "A", 0},
      {RuleReferenceKind::Keyword, "B", 0},
      {RuleReferenceKind::Form, "Mod.esp", 0x800},
  };
  REQUIRE(rules.References() == expected);
  REQUIRE(rules.ImpressionCount() == 2);
}

TEST_CASE("RulesProjectThroughTemperaments") {
  RuleBook rules;
  REQUIRE(Loaded(rules, banditRules));

  // Nothing matches until the host binds a reference.
  const std::array<std::uint32_t, 1> cuirass = {banditCuirass};
  const auto unbound = rules.Evaluate(NpcProfile::Guard, cuirass);
  REQUIRE((unbound.matchedRules == 0 && Target(unbound, Axis::Suspicion) == 0.0F));
  REQUIRE(rules.BoundCount() == 0);

  BindLikeAHost(rules);
  REQUIRE(rules.BoundCount() == 2);

  const auto guard = rules.Evaluate(NpcProfile::Guard, cuirass);
  REQUIRE((Target(guard, Axis::Suspicion) == 40.0F && Target(guard, Axis::Fear) == 0.0F && guard.matchedRules == 1));

  const auto citizen = rules.Evaluate(NpcProfile::Citizen, cuirass);
  REQUIRE((Target(citizen, Axis::Suspicion) == 20.0F && Target(citizen, Axis::Fear) == 20.0F));

  // A profile without a temperament entry is unaffected, though the rules match.
  const std::array<std::uint32_t, 2> cuirassAndHide = {banditCuirass, hideKeyword};
  const auto follower = rules.Evaluate(NpcProfile::Follower, cuirassAndHide);
  REQUIRE(follower.matchedRules == 2);
  for (const auto target : follower.targets) {
    REQUIRE(target == 0.0F);
  }
}

TEST_CASE("TheStrongestRuleWinsInsteadOfAddingUp") {
  RuleBook rules;
  REQUIRE(Loaded(rules, banditRules));
  BindLikeAHost(rules);

  const std::array<std::uint32_t, 2> hideOnly = {0x1234, hideKeyword};
  REQUIRE(Target(rules.Evaluate(NpcProfile::Guard, hideOnly), Axis::Suspicion) == 15.0F);

  const std::array<std::uint32_t, 2> both = {banditCuirass, hideKeyword};
  const auto outcome = rules.Evaluate(NpcProfile::Guard, both);
  REQUIRE((Target(outcome, Axis::Suspicion) == 40.0F && outcome.matchedRules == 2));

  const std::array<std::uint32_t, 2> unrelated = {0x1234, ironKeyword};
  const auto none = rules.Evaluate(NpcProfile::Guard, unrelated);
  REQUIRE((none.matchedRules == 0 && Target(none, Axis::Suspicion) == 0.0F));
}

TEST_CASE("UnbindingStopsAReferenceMatching") {
  RuleBook rules;
  REQUIRE(Loaded(rules, banditRules));
  BindLikeAHost(rules);
  REQUIRE(rules.Bind(0, 0));

  const std::array<std::uint32_t, 1> cuirass = {banditCuirass};
  REQUIRE(Target(rules.Evaluate(NpcProfile::Guard, cuirass), Axis::Suspicion) == 0.0F);
  REQUIRE(rules.BoundCount() == 1);
  REQUIRE(!rules.Bind(99, 1));
}

TEST_CASE("ALaterFileRetunesTemperaments") {
  RuleBook rules;
  REQUIRE(Loaded(rules, banditRules));
  BindLikeAHost(rules);
  REQUIRE(Loaded(rules, R"({ "temperaments": { "guard": { "banditry": { "fear": 0.5 } } } })"));

  const std::array<std::uint32_t, 1> cuirass = {banditCuirass};
  const auto guard = rules.Evaluate(NpcProfile::Guard, cuirass);
  REQUIRE((Target(guard, Axis::Suspicion) == 0.0F && Target(guard, Axis::Fear) == 20.0F));
}

TEST_CASE("InvalidRuleFilesAreRejectedWhole") {
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
    REQUIRE(Loaded(rules, banditRules));
    BindLikeAHost(rules);
    const auto references = rules.References().size();
    const auto impressions = rules.ImpressionCount();

    const auto rejection = rules.LoadJson(entry.json);
    INFO("rule file: ", std::string{entry.json});
    INFO("expected a rejection mentioning: ", std::string{entry.reason});
    INFO("got: ", rejection.value_or("(accepted)"));
    REQUIRE(rejection.has_value());
    REQUIRE(rejection->find(entry.reason) != std::string::npos);
    REQUIRE((rules.References().size() == references && rules.ImpressionCount() == impressions));
    const std::array<std::uint32_t, 1> cuirass = {banditCuirass};
    REQUIRE(Target(rules.Evaluate(NpcProfile::Guard, cuirass), Axis::Suspicion) == 40.0F);
  }

  // Empty, null and absent sections are fine; unrelated fields are not.
  RuleBook rules;
  REQUIRE(Loaded(rules, "{}"));
  REQUIRE(Loaded(rules, R"({ "impressions": [], "temperaments": {} })"));
  REQUIRE(Loaded(rules, R"({ "impressions": null, "temperaments": null })"));
  REQUIRE(Loaded(rules, R"({ "impressions": [ { "name": null, "keywords": null, "forms": ["0x10~A.esp"], "projects": { "x": 1 } } ] })"));
  REQUIRE(rules.References().size() == 1);
}

// The store's view of the same rules, driven the way ImpressionRules.cpp drives
// them: load, list references, bind the ones the game resolves, then observe.
TEST_CASE("ImpressionsRaiseMemory") {
  MemoryStore store;
  std::string_view broken = R"({ "temperaments": { "guard": { "banditry": { "anger": 1 } } } })";
  const auto reason = store.LoadRules(broken);
  REQUIRE((reason && reason->find("unknown axis") != std::string::npos));
  REQUIRE(store.LoadRules("")); // empty is refused too
  REQUIRE(!store.LoadRules(banditRules));

  const auto summary = store.GetRulesSummary();
  REQUIRE((summary.impressions == 2 && summary.references == 2 && summary.boundReferences == 0));
  REQUIRE(!store.GetRuleReference(2));
  const auto keyword = store.GetRuleReference(1);
  REQUIRE((keyword && keyword->kind == RuleReferenceKind::Keyword && keyword->name == "ArmorMaterialHide"));
  REQUIRE(store.BindRuleReference(1, hideKeyword) == MemoryStatus::Ok);
  REQUIRE(store.BindRuleReference(7, hideKeyword) == MemoryStatus::NotFound);
  REQUIRE(store.GetRulesSummary().boundReferences == 1);

  const std::array hideCuirass = {Item(0x13911)};
  const std::array<std::uint32_t, 1> keywords = {hideKeyword};
  ImpressionResult impression;

  // A memory must exist first.
  REQUIRE(store.ObserveImpression(123, 10.0, NpcProfile::Guard, hideCuirass, keywords, impression) == MemoryStatus::NotFound);
  Know(store, 123, 10.0);

  REQUIRE(store.ObserveImpression(123, 10.0, NpcProfile::Guard, hideCuirass, keywords, impression) == MemoryStatus::Ok);
  REQUIRE((impression.raisedAxes == (1U << AxisSlot(Axis::Suspicion)) && impression.matchedRules == 1));
  REQUIRE(impression.targets[AxisSlot(Axis::Suspicion)] == 15.0F);

  // Worn again on the next scan: the level holds and nothing is reported twice.
  for (int scan = 0; scan < 10; ++scan) {
    REQUIRE(store.ObserveImpression(123, 10.0, NpcProfile::Guard, hideCuirass, keywords, impression) == MemoryStatus::Ok);
    REQUIRE(impression.raisedAxes == 0);
  }
  REQUIRE(Current(store, 123, Axis::Suspicion, 10.0) == 15.0F);

  // A profile with no temperament for it feels nothing.
  REQUIRE(store.ObserveImpression(123, 10.0, NpcProfile::Follower, hideCuirass, keywords, impression) == MemoryStatus::Ok);
  REQUIRE(impression.targets[AxisSlot(Axis::Suspicion)] == 0.0F);

  // Bad arguments.
  REQUIRE(store.ObserveImpression(123, 10.0, static_cast<NpcProfile>(99), hideCuirass, keywords, impression) == MemoryStatus::InvalidArgument);
  REQUIRE(store.ObserveImpression(123, -1.0, NpcProfile::Guard, hideCuirass, keywords, impression) == MemoryStatus::InvalidArgument);
  REQUIRE(store.ObserveImpression(0, 10.0, NpcProfile::Guard, hideCuirass, keywords, impression) == MemoryStatus::InvalidId);
  const std::vector<std::uint32_t> tooManyKeywords(maxWornKeywords + 1, hideKeyword);
  REQUIRE(store.ObserveImpression(123, 10.0, NpcProfile::Guard, hideCuirass, tooManyKeywords, impression) == MemoryStatus::InvalidArgument);

  // Rules belong to the session: clearing memories leaves them bound.
  store.Clear();
  REQUIRE(store.GetRulesSummary().boundReferences == 1);
}

TEST_CASE("ImpressionsNeverLowerAndTopUpQuietly") {
  MemoryStore store;
  REQUIRE(!store.LoadRules(banditRules));
  for (std::uint32_t index = 0; index < 2; ++index) {
    const auto reference = store.GetRuleReference(index);
    store.BindRuleReference(index, reference->kind == RuleReferenceKind::Form ? reference->localFormId : hideKeyword);
  }
  const std::array outfit = {Item(banditCuirass)};
  ImpressionResult impression;

  // Raises to the level, never accumulates.
  Know(store, 1, 1.0);
  REQUIRE(store.ObserveImpression(1, 1.0, NpcProfile::Guard, outfit, {}, impression) == MemoryStatus::Ok);
  REQUIRE(impression.raisedAxes == (1U << AxisSlot(Axis::Suspicion)));
  REQUIRE(Current(store, 1, Axis::Suspicion) == 40.0F);

  // One game hour of a 48-hour half-life is well under one point of decay: the
  // top-up is applied but is not news.
  REQUIRE(store.ObserveImpression(1, 1.0 + 1.0 / 24.0, NpcProfile::Guard, outfit, {}, impression) == MemoryStatus::Ok);
  REQUIRE(impression.raisedAxes == 0);
  REQUIRE(Current(store, 1, Axis::Suspicion, 1.0 + 1.0 / 24.0) == 40.0F);

  // A stronger feeling from an earlier event survives an outfit that implies less.
  Know(store, 2, 1.0);
  MemoryDelta delta{};
  Slot(delta, Axis::Suspicion) = 90.0F;
  MemorySnapshot snapshot;
  store.ApplyDelta(2, 1.0, delta, snapshot);
  REQUIRE(store.ObserveImpression(2, 1.0, NpcProfile::Guard, outfit, {}, impression) == MemoryStatus::Ok);
  REQUIRE(impression.raisedAxes == 0);
  REQUIRE(Current(store, 2, Axis::Suspicion) == 90.0F);
}

TEST_CASE("ProfilesMirrorTheHostEnum") {
  // The rule file names and the host enum must agree on every profile.
  RuleBook rules;
  constexpr std::array<const char *, npcProfileCount> names = {"guard", "follower", "citizen", "merchant", "generic"};
  for (std::size_t index = 0; index < names.size(); ++index) {
    REQUIRE(Loaded(rules, std::string{R"({ "temperaments": { ")"} + names[index] + R"(": { "x": { "fear": 1 } } } })"));
    REQUIRE(static_cast<std::size_t>(static_cast<NpcProfile>(index)) == index);
  }
  REQUIRE(!Loaded(rules, R"({ "temperaments": { "hostile": {} } })"));
}

TEST_SUITE_END();
