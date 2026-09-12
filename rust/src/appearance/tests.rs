use super::*;
use crate::memory::{Axis, NpcMemory};
use crate::tuning::{Limits, Tuning};

fn appearance_item(form_id: u32, slot_mask: u32) -> AppearanceItem {
    AppearanceItem { form_id, slot_mask }
}

fn similarity(left: &[AppearanceItem], right: &[AppearanceItem]) -> f32 {
    outfit_similarity(left, right, &AppearanceWeights::default())
}

#[test]
fn covered_face_requires_a_matching_outfit() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0);

    let first = store
        .observe_appearance(guard, vec![appearance_item(10, 1 << 2)], false, false, 1.0)
        .unwrap();
    assert!(!first.appearance_matched);
    assert!(!first.recognized);
    assert!(first.learned);

    let same = store
        .observe_appearance(guard, vec![appearance_item(10, 1 << 2)], false, false, 1.1)
        .unwrap();
    assert!(same.appearance_matched);
    assert!(!same.recognized);
    assert!(!same.learned);
    assert_eq!(same.outfit_similarity, 1.0);

    let disguise = store
        .observe_appearance(guard, vec![appearance_item(20, 1 << 2)], false, false, 1.2)
        .unwrap();
    assert!(!disguise.recognized);
    assert_eq!(disguise.identity_confidence, 0.0);
    assert_eq!(disguise.known_appearances, 1);
}

#[test]
fn a_known_visible_face_connects_a_completely_new_outfit() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0);
    store
        .observe_appearance(guard, vec![appearance_item(10, 1 << 2)], true, false, 1.0)
        .unwrap();

    let changed = store
        .observe_appearance(guard, vec![appearance_item(20, 1 << 2)], true, false, 1.1)
        .unwrap();

    assert!(changed.recognized);
    assert!(changed.learned);
    assert_eq!(changed.outfit_similarity, 0.0);
    assert_eq!(changed.identity_confidence, 1.0);
    assert_eq!(changed.known_appearances, 2);
}

#[test]
fn revealing_the_face_links_an_anonymous_outfit_to_the_player() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0);
    let outfit = vec![appearance_item(10, 1 << 2)];

    let hidden = store
        .observe_appearance(guard, outfit.clone(), false, false, 1.0)
        .unwrap();
    assert!(!hidden.recognized);

    let revealed = store
        .observe_appearance(guard, outfit.clone(), true, false, 1.1)
        .unwrap();
    assert!(revealed.recognized);
    assert!(revealed.appearance_matched);

    let hidden_again = store
        .observe_appearance(guard, outfit, false, false, 1.2)
        .unwrap();
    assert!(hidden_again.recognized);
    assert!(hidden_again.appearance_matched);
}

#[test]
fn uninterrupted_visual_contact_links_a_completely_different_hidden_outfit() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0);
    store
        .observe_appearance(guard, vec![appearance_item(10, 1 << 2)], true, false, 1.0)
        .unwrap();

    let changed = store
        .observe_appearance(guard, vec![appearance_item(20, 1 << 2)], false, true, 1.1)
        .unwrap();

    assert!(changed.recognized);
    assert!(changed.linked_by_continuity);
    assert!(changed.learned);
    assert_eq!(changed.outfit_similarity, 0.0);

    let seen_later = store
        .observe_appearance(guard, vec![appearance_item(20, 1 << 2)], false, false, 1.2)
        .unwrap();
    assert!(seen_later.recognized);
    assert!(!seen_later.linked_by_continuity);
}

#[test]
fn a_hidden_outfit_change_is_not_linked_after_visual_contact_is_lost() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0);
    store
        .observe_appearance(guard, vec![appearance_item(10, 1 << 2)], true, false, 1.0)
        .unwrap();

    let changed = store
        .observe_appearance(guard, vec![appearance_item(20, 1 << 2)], false, false, 1.1)
        .unwrap();

    assert!(!changed.recognized);
    assert!(!changed.linked_by_continuity);
    assert!(!changed.learned);
}

#[test]
fn host_verified_visual_contact_can_establish_identity() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0);

    let hidden = store
        .observe_appearance(guard, vec![appearance_item(10, 1 << 2)], false, true, 1.0)
        .unwrap();

    assert!(hidden.recognized);
    assert!(hidden.linked_by_continuity);
}

#[test]
fn evaluating_an_appearance_has_no_side_effects() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0);

    let evaluation = store
        .evaluate_appearance(guard, vec![appearance_item(10, 1 << 2)], true, false)
        .unwrap();

    assert!(evaluation.recognized);
    assert!(!evaluation.learned);
    assert_eq!(evaluation.known_appearances, 0);
    assert!(store.known_appearances(guard).is_empty());
}

#[test]
fn two_empty_outfits_are_identical() {
    assert_eq!(similarity(&[], &[]), 1.0);
}

#[test]
fn outfit_similarity_is_weighted_by_visible_slot_groups() {
    let body = 1 << 2;
    let hands = 1 << 3;
    let left = [appearance_item(10, body), appearance_item(20, hands)];
    let right = [appearance_item(10, body), appearance_item(30, hands)];

    // Matching body weight 0.35 over applicable body+hands weight 0.45.
    assert!((similarity(&left, &right) - (0.35 / 0.45)).abs() < 0.0001);
}

#[test]
fn reweighting_a_category_changes_the_similarity_it_produces() {
    let body = 1 << 2;
    let hands = 1 << 3;
    let left = [appearance_item(10, body), appearance_item(20, hands)];
    let right = [appearance_item(10, body), appearance_item(30, hands)];
    let weights = AppearanceWeights {
        hands: 0.35,
        ..AppearanceWeights::default()
    };

    let result = outfit_similarity(&left, &right, &weights);

    assert!((result - 0.5).abs() < 0.0001);
}

#[test]
fn normalizing_merges_slots_of_one_form_and_drops_empty_entries() {
    let mut items = vec![
        appearance_item(20, 1 << 3),
        appearance_item(0, 1 << 2),
        appearance_item(10, 1 << 2),
        appearance_item(10, 1 << 16),
        appearance_item(30, 0),
    ];

    normalize_items(&mut items);

    assert_eq!(
        items,
        vec![
            appearance_item(10, (1 << 2) | (1 << 16)),
            appearance_item(20, 1 << 3),
        ]
    );
}

#[test]
fn changing_only_headwear_matches_at_the_initial_seventy_percent_threshold() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0);
    let first = vec![
        appearance_item(1, 1 << 7),
        appearance_item(2, 1 << 3),
        appearance_item(3, 1 << 2),
        appearance_item(4, (1 << 1) | (1 << 12)),
        appearance_item(5, 1 << 22),
        appearance_item(6, 1 << 6),
    ];
    let mut helmet_changed = first.clone();
    helmet_changed[3].form_id = 7;
    store.restore_entry(
        guard,
        NpcMemory::new(1.0),
        vec![KnownAppearance {
            items: first,
            face_visible: false,
            identity_known: true,
            last_seen_game_days: 1.0,
        }],
    );

    let result = store
        .observe_appearance(guard, helmet_changed, false, false, 1.1)
        .unwrap();

    assert!((result.outfit_similarity - 0.705_882_3).abs() < 0.0001);
    assert!((result.recognition_threshold - 0.70).abs() < 0.0001);
    assert!(result.recognized);
    assert!(result.learned);
}

#[test]
fn recognition_reduces_the_threshold_from_seventy_to_fifty_percent() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0)[Axis::Recognition] = 100.0;

    let first = store
        .observe_appearance(guard, vec![appearance_item(10, 1 << 2)], false, false, 1.0)
        .unwrap();

    assert!((first.recognition_threshold - 0.50).abs() < 0.0001);
}

#[test]
fn the_oldest_outfit_is_evicted_at_the_configured_capacity() {
    let tuning = Tuning {
        limits: Limits {
            max_known_appearances: 2,
            ..Limits::default()
        },
        ..Tuning::default()
    };
    let mut store = MemoryStore::with_tuning(tuning);
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0);

    for (index, day) in [(10_u32, 1.0), (20, 2.0), (30, 3.0)] {
        store
            .observe_appearance(
                guard,
                vec![appearance_item(index, 1 << 2)],
                true,
                false,
                day,
            )
            .unwrap();
    }

    let known = store.known_appearances(guard);
    assert_eq!(known.len(), 2);
    assert!(known
        .iter()
        .all(|appearance| appearance.items[0].form_id != 10));
}

#[test]
fn an_observation_over_the_item_limit_is_rejected() {
    let tuning = Tuning {
        limits: Limits {
            max_appearance_items: 2,
            ..Limits::default()
        },
        ..Tuning::default()
    };
    let mut store = MemoryStore::with_tuning(tuning);
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0);
    let too_many = vec![
        appearance_item(10, 1 << 2),
        appearance_item(20, 1 << 3),
        appearance_item(30, 1 << 7),
    ];

    assert!(store
        .observe_appearance(guard, too_many.clone(), true, false, 1.0)
        .is_none());
    assert!(store
        .evaluate_appearance(guard, too_many, true, false)
        .is_none());
}

#[test]
fn technical_decapitation_slots_leave_the_visual_signature() {
    let mut items = vec![
        // Only technical bits: the piece carries no visual information.
        appearance_item(10, TECHNICAL_SLOTS),
        // Mixed: the body bit survives, the technical bits do not.
        appearance_item(20, (1 << 2) | (1 << 20)),
        // Untouched.
        appearance_item(30, 1 << 10),
    ];

    normalize_items(&mut items);

    assert_eq!(
        items,
        vec![appearance_item(20, 1 << 2), appearance_item(30, 1 << 10)]
    );
}

#[test]
fn tail_and_long_hair_are_not_technical_slots() {
    let mut items = vec![appearance_item(10, (1 << 10) | (1 << 11))];

    normalize_items(&mut items);

    assert_eq!(items, vec![appearance_item(10, (1 << 10) | (1 << 11))]);
}

#[test]
fn an_outfit_saved_with_technical_bits_still_matches_a_fresh_capture() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0);
    // Restored the way an older save would have written it.
    store.restore_entry(
        guard,
        NpcMemory::new(1.0),
        vec![KnownAppearance {
            items: vec![appearance_item(10, (1 << 2) | (1 << 20))],
            face_visible: true,
            identity_known: true,
            last_seen_game_days: 1.0,
        }],
    );

    let seen_again = store
        .observe_appearance(guard, vec![appearance_item(10, 1 << 2)], true, false, 1.5)
        .unwrap();

    assert_eq!(seen_again.outfit_similarity, 1.0);
    assert!(seen_again.appearance_matched);
    assert!(!seen_again.learned, "the same outfit must not be relearned");
    assert_eq!(store.known_appearances(guard).len(), 1);
}

#[test]
fn repeated_identical_observation_does_not_keep_refreshing_the_outfit() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0);
    let outfit = vec![appearance_item(10, 1 << 2)];
    store
        .observe_appearance(guard, outfit.clone(), true, false, 1.0)
        .unwrap();

    // A whole burst of polling inside one game hour changes nothing.
    for tick in 1..=20 {
        let result = store
            .observe_appearance(
                guard,
                outfit.clone(),
                true,
                false,
                1.0 + f64::from(tick) * 0.0005,
            )
            .unwrap();
        assert!(!result.learned);
    }

    assert_eq!(store.known_appearances(guard).len(), 1);
    assert_eq!(store.known_appearances(guard)[0].last_seen_game_days, 1.0);
}

#[test]
fn an_outfit_is_refreshed_once_the_interval_has_really_passed() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0);
    let outfit = vec![appearance_item(10, 1 << 2)];
    store
        .observe_appearance(guard, outfit.clone(), true, false, 1.0)
        .unwrap();

    store
        .observe_appearance(guard, outfit, true, false, 1.5)
        .unwrap();

    assert_eq!(store.known_appearances(guard)[0].last_seen_game_days, 1.5);
}

#[test]
fn polling_never_makes_the_newest_outfit_immune_to_eviction() {
    let tuning = Tuning {
        limits: Limits {
            max_known_appearances: 2,
            ..Limits::default()
        },
        ..Tuning::default()
    };
    let mut store = MemoryStore::with_tuning(tuning);
    let guard = NpcId::new(123).unwrap();
    store.get_or_create_at(guard, 1.0);

    store
        .observe_appearance(guard, vec![appearance_item(10, 1 << 2)], true, false, 1.0)
        .unwrap();
    store
        .observe_appearance(guard, vec![appearance_item(20, 1 << 2)], true, false, 2.0)
        .unwrap();
    // The Player stands still in outfit 10 while the scan polls repeatedly.
    for tick in 0..50 {
        store
            .observe_appearance(
                guard,
                vec![appearance_item(10, 1 << 2)],
                true,
                false,
                2.0 + f64::from(tick) * 0.0005,
            )
            .unwrap();
    }
    store
        .observe_appearance(guard, vec![appearance_item(30, 1 << 2)], true, false, 3.0)
        .unwrap();

    let known = store.known_appearances(guard);
    assert_eq!(known.len(), 2);
    assert!(
        known.iter().all(|entry| entry.items[0].form_id != 10),
        "the oldest outfit must still be the one evicted"
    );
}
