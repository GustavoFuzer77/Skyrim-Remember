use super::*;

const SHIPPED_RULES: &str =
    include_str!("../../../data/SKSE/Plugins/SkyrimRemember/SkyrimRemember_Rules.json");

const BANDIT_CUIRASS: u32 = 0x0006_F393;
const HIDE_KEYWORD: u32 = 0x0100_0001;
const IRON_KEYWORD: u32 = 0x0100_0002;

fn item(form_id: u32) -> AppearanceItem {
    AppearanceItem {
        form_id,
        slot_mask: 1 << 2,
    }
}

const BANDIT_RULES: &str = r#"{
  "impressions": [
    { "name": "bandit", "forms": ["0x06F393~Skyrim.esm"], "projects": { "banditry": 40 } },
    { "keywords": ["ArmorMaterialHide"], "projects": { "banditry": 15 } }
  ],
  "temperaments": {
    "guard": { "banditry": { "suspicion": 1.0 } },
    "citizen": { "banditry": { "suspicion": 0.5, "fear": 0.5 } }
  }
}"#;

/// Loads the rules and binds them as a host would: the bandit cuirass to its
/// FormID and the hide keyword to a made-up one.
fn bound_rules(json: &str) -> Rules {
    let mut rules = Rules::default();
    rules.load_json(json.as_bytes()).unwrap();
    for index in 0..rules.references().len() {
        let runtime = match &rules.references()[index] {
            RuleReference::Form { local_id, .. } => *local_id,
            RuleReference::Keyword(name) if name == "ArmorMaterialHide" => HIDE_KEYWORD,
            RuleReference::Keyword(_) => IRON_KEYWORD,
        };
        assert!(rules.bind(index, runtime));
    }
    rules
}

#[test]
fn the_shipped_rule_file_is_valid() {
    let mut rules = Rules::default();
    rules.load_json(SHIPPED_RULES.as_bytes()).unwrap();

    assert!(rules.impression_count() >= 1);
    assert!(rules.references().contains(&RuleReference::Form {
        plugin: "Skyrim.esm".to_owned(),
        local_id: BANDIT_CUIRASS,
    }));
}

#[test]
fn references_are_listed_once_in_load_order() {
    let mut rules = Rules::default();
    rules
        .load_json(
            br#"{ "impressions": [
                { "keywords": ["A", "B"], "projects": { "x": 1 } },
                { "keywords": ["B"], "forms": ["800~Mod.esp"], "projects": { "x": 2 } }
            ] }"#,
        )
        .unwrap();

    assert_eq!(
        rules.references(),
        &[
            RuleReference::Keyword("A".to_owned()),
            RuleReference::Keyword("B".to_owned()),
            RuleReference::Form {
                plugin: "Mod.esp".to_owned(),
                local_id: 0x800,
            },
        ]
    );
}

#[test]
fn nothing_matches_until_the_host_binds_a_reference() {
    let mut rules = Rules::default();
    rules.load_json(BANDIT_RULES.as_bytes()).unwrap();

    let outcome = rules.evaluate(Profile::Guard, &[item(BANDIT_CUIRASS)], &[]);

    assert_eq!(outcome, ImpressionOutcome::default());
    assert_eq!(rules.bound_count(), 0);
}

#[test]
fn a_worn_form_projects_through_the_profile_temperament() {
    let rules = bound_rules(BANDIT_RULES);

    let guard = rules.evaluate(Profile::Guard, &[item(BANDIT_CUIRASS)], &[]);
    assert_eq!(guard.targets[Axis::Suspicion.index()], 40.0);
    assert_eq!(guard.targets[Axis::Fear.index()], 0.0);
    assert_eq!(guard.matched_rules, 1);

    let citizen = rules.evaluate(Profile::Citizen, &[item(BANDIT_CUIRASS)], &[]);
    assert_eq!(citizen.targets[Axis::Suspicion.index()], 20.0);
    assert_eq!(citizen.targets[Axis::Fear.index()], 20.0);
}

#[test]
fn a_profile_without_a_temperament_entry_is_unaffected() {
    let rules = bound_rules(BANDIT_RULES);

    let follower = rules.evaluate(Profile::Follower, &[item(BANDIT_CUIRASS)], &[HIDE_KEYWORD]);

    assert_eq!(follower.targets, [0.0; AXIS_CAPACITY]);
    assert_eq!(follower.matched_rules, 2);
}

#[test]
fn keywords_match_and_the_strongest_rule_wins_instead_of_adding_up() {
    let rules = bound_rules(BANDIT_RULES);

    let hide_only = rules.evaluate(Profile::Guard, &[item(0x1234)], &[HIDE_KEYWORD]);
    assert_eq!(hide_only.targets[Axis::Suspicion.index()], 15.0);

    let both = rules.evaluate(Profile::Guard, &[item(BANDIT_CUIRASS)], &[HIDE_KEYWORD]);
    assert_eq!(both.targets[Axis::Suspicion.index()], 40.0);
    assert_eq!(both.matched_rules, 2);

    let unrelated = rules.evaluate(Profile::Guard, &[item(0x1234)], &[IRON_KEYWORD]);
    assert_eq!(unrelated.targets, [0.0; AXIS_CAPACITY]);
}

#[test]
fn unbinding_a_reference_stops_it_matching() {
    let mut rules = bound_rules(BANDIT_RULES);
    assert!(rules.bind(0, 0));

    let outcome = rules.evaluate(Profile::Guard, &[item(BANDIT_CUIRASS)], &[]);

    assert_eq!(outcome.targets[Axis::Suspicion.index()], 0.0);
    assert!(!rules.bind(99, 1));
}

#[test]
fn a_later_file_retunes_a_temperament_and_adds_impressions() {
    let mut rules = bound_rules(BANDIT_RULES);
    rules
        .load_json(br#"{ "temperaments": { "guard": { "banditry": { "fear": 0.5 } } } }"#)
        .unwrap();

    let guard = rules.evaluate(Profile::Guard, &[item(BANDIT_CUIRASS)], &[]);

    assert_eq!(guard.targets[Axis::Suspicion.index()], 0.0);
    assert_eq!(guard.targets[Axis::Fear.index()], 20.0);
}

#[test]
fn invalid_files_are_rejected_whole_with_a_readable_reason() {
    let cases: [(&str, &str); 8] = [
        ("{ not json", "key must be a string"),
        (r#"{ "impresions": [] }"#, "unknown field"),
        (
            r#"{ "impressions": [ { "projects": { "x": 1 } } ] }"#,
            "at least one keyword or form",
        ),
        (
            r#"{ "impressions": [ { "keywords": ["A"], "projects": { "x": 150 } } ] }"#,
            "strength 150",
        ),
        (
            r#"{ "impressions": [ { "forms": ["0x06F393"], "projects": { "x": 1 } } ] }"#,
            "must look like",
        ),
        (
            r#"{ "impressions": [ { "forms": ["0x1000000~A.esp"], "projects": { "x": 1 } } ] }"#,
            "local id",
        ),
        (
            r#"{ "temperaments": { "bandit": { "x": { "fear": 1 } } } }"#,
            "unknown profile \"bandit\"",
        ),
        (
            r#"{ "temperaments": { "guard": { "x": { "anger": 1 } } } }"#,
            "unknown axis \"anger\"",
        ),
    ];
    for (json, expected) in cases {
        let mut rules = bound_rules(BANDIT_RULES);
        let before = rules.references().len();
        let rejection = rules.load_json(json.as_bytes()).unwrap_err().to_string();
        assert!(
            rejection.contains(expected),
            "{json}: expected \"{expected}\" in \"{rejection}\""
        );
        assert_eq!(rules.references().len(), before, "{json} changed the rules");
        let guard = rules.evaluate(Profile::Guard, &[item(BANDIT_CUIRASS)], &[]);
        assert_eq!(guard.targets[Axis::Suspicion.index()], 40.0, "{json}");
    }
}

#[test]
fn an_impression_raises_memory_to_its_level_without_accumulating() {
    let mut store = MemoryStore::new();
    *store.rules_mut() = bound_rules(BANDIT_RULES);
    let guard = NpcId::new(0x123).unwrap();
    store.register(guard, 1.0);
    let outfit = [item(BANDIT_CUIRASS)];

    let first = store
        .observe_impression(guard, Profile::Guard, &outfit, &[], 1.0)
        .unwrap();
    assert_eq!(first.raised_axes, 1 << Axis::Suspicion.index());
    assert_eq!(store.get(guard).unwrap()[Axis::Suspicion], 40.0);

    for _ in 0..10 {
        let again = store
            .observe_impression(guard, Profile::Guard, &outfit, &[], 1.0)
            .unwrap();
        assert_eq!(again.raised_axes, 0);
    }
    assert_eq!(store.get(guard).unwrap()[Axis::Suspicion], 40.0);
}

#[test]
fn a_small_top_up_after_decay_is_applied_but_not_reported() {
    let mut store = MemoryStore::new();
    *store.rules_mut() = bound_rules(BANDIT_RULES);
    let guard = NpcId::new(0x123).unwrap();
    store.register(guard, 1.0);
    let outfit = [item(BANDIT_CUIRASS)];
    store.observe_impression(guard, Profile::Guard, &outfit, &[], 1.0);

    // One game hour of a 48-hour half-life: well under one point of decay.
    let later = store
        .observe_impression(guard, Profile::Guard, &outfit, &[], 1.0 + 1.0 / 24.0)
        .unwrap();

    assert_eq!(later.raised_axes, 0);
    assert_eq!(store.get(guard).unwrap()[Axis::Suspicion], 40.0);
}

#[test]
fn an_impression_never_lowers_a_stronger_feeling() {
    let mut store = MemoryStore::new();
    *store.rules_mut() = bound_rules(BANDIT_RULES);
    let guard = NpcId::new(0x123).unwrap();
    store.register(guard, 1.0);
    store.get_mut(guard).unwrap()[Axis::Suspicion] = 90.0;

    let outcome = store
        .observe_impression(guard, Profile::Guard, &[item(BANDIT_CUIRASS)], &[], 1.0)
        .unwrap();

    assert_eq!(outcome.raised_axes, 0);
    assert_eq!(store.get(guard).unwrap()[Axis::Suspicion], 90.0);
}

#[test]
fn impressions_need_an_existing_memory_and_survive_a_clear() {
    let mut store = MemoryStore::new();
    *store.rules_mut() = bound_rules(BANDIT_RULES);
    let stranger = NpcId::new(0x456).unwrap();

    assert!(store
        .observe_impression(stranger, Profile::Guard, &[item(BANDIT_CUIRASS)], &[], 1.0)
        .is_none());

    store.clear();
    assert_eq!(store.rules().bound_count(), 2);
}

#[test]
fn profiles_mirror_the_host_enum() {
    for (index, profile) in Profile::ALL.into_iter().enumerate() {
        assert_eq!(profile as usize, index);
        assert_eq!(Profile::from_u32(index as u32), Some(profile));
    }
    assert_eq!(Profile::from_u32(5), None);
}
