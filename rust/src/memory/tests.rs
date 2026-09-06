use super::*;
use crate::tuning::Limits;

#[test]
fn npc_id_rejects_the_null_form_id() {
    assert_eq!(NpcId::new(0), None);
    assert_eq!(NpcId::new(0x000A_1234).map(NpcId::get), Some(0x000A_1234));
}

#[test]
fn store_creates_memory_with_zeroed_values() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(0x000A_1234).expect("the test FormID is valid");

    let memory = store.get_or_create(guard);

    assert_eq!(*memory, NpcMemory::default());
    assert_eq!(store.len(), 1);
}

#[test]
fn fear_uses_a_seventy_two_hour_half_life() {
    let mut memory = NpcMemory::new(100.0);
    memory.fear = 80.0;

    memory.apply_decay(103.0, DecayHalfLives::default());

    assert!((memory.fear - 40.0).abs() < 0.0001);
    assert_eq!(memory.last_decay_game_days, 103.0);
}

#[test]
fn decay_ignores_time_rollback_and_deltas_are_clamped() {
    let mut memory = NpcMemory::new(20.0);
    memory.fear = 40.0;
    memory.apply_decay(19.0, DecayHalfLives::default());
    assert_eq!(memory.fear, 40.0);
    assert_eq!(memory.last_decay_game_days, 20.0);

    memory.add(200.0, -10.0, -100.0, 50.0);
    assert_eq!(memory.recognition, 100.0);
    assert_eq!(memory.suspicion, 0.0);
    assert_eq!(memory.fear, 0.0);
    assert_eq!(memory.respect, 50.0);
}

#[test]
fn register_reports_creation_only_the_first_time_and_decays_afterwards() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).expect("the test FormID is valid");

    assert!(store.register(guard, 100.0));
    store.get_mut(guard).expect("just created").fear = 80.0;

    assert!(!store.register(guard, 103.0));
    let memory = store.get(guard).expect("still present");
    assert!((memory.fear - 40.0).abs() < 0.0001);
    assert_eq!(memory.last_decay_game_days, 103.0);
}

#[test]
fn touch_never_creates_a_memory() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).expect("the test FormID is valid");

    assert!(store.touch(guard, 1.0).is_none());
    assert!(store.is_empty());
}

#[test]
fn the_store_decays_with_its_own_tuning_rather_than_the_defaults() {
    let tuning = Tuning {
        decay: DecayHalfLives {
            fear_hours: 24.0,
            ..DecayHalfLives::default()
        },
        ..Tuning::default()
    };
    let mut store = MemoryStore::with_tuning(tuning);
    let guard = NpcId::new(123).expect("the test FormID is valid");
    store.get_or_create_at(guard, 1.0).fear = 80.0;

    let memory = store.touch(guard, 2.0).expect("the memory exists");

    assert!((memory.fear - 40.0).abs() < 0.0001);
}

#[test]
fn replacing_the_data_keeps_the_configured_tuning() {
    let tuning = Tuning {
        limits: Limits {
            max_known_appearances: 3,
            ..Limits::default()
        },
        ..Tuning::default()
    };
    let mut store = MemoryStore::with_tuning(tuning);

    store.replace(MemoryStore::new());

    assert_eq!(store.tuning().limits.max_known_appearances, 3);
}

#[test]
fn getting_an_existing_memory_preserves_its_values_and_store_size() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(0x000A_1234).expect("the test FormID is valid");

    store.get_or_create(guard).recognition = 42.0;
    let memory = store.get_or_create(guard);

    assert_eq!(memory.recognition, 42.0);
    assert_eq!(store.len(), 1);
}

#[test]
fn memories_are_individual_per_actor_reference() {
    let mut store = MemoryStore::new();
    let guard_a = NpcId::new(0x000A_1234).expect("the test FormID is valid");
    let guard_b = NpcId::new(0x000A_5678).expect("the test FormID is valid");

    store.get_or_create(guard_a).suspicion = 35.0;
    store.get_or_create(guard_b).fear = 20.0;

    assert_eq!(
        store.get(guard_a).map(|memory| memory.suspicion),
        Some(35.0)
    );
    assert_eq!(store.get(guard_a).map(|memory| memory.fear), Some(0.0));
    assert_eq!(store.get(guard_b).map(|memory| memory.suspicion), Some(0.0));
    assert_eq!(store.get(guard_b).map(|memory| memory.fear), Some(20.0));
}

#[test]
fn store_can_remove_and_clear_memories() {
    let mut store = MemoryStore::new();
    let guard_a = NpcId::new(0x000A_1234).expect("the test FormID is valid");
    let guard_b = NpcId::new(0x000A_5678).expect("the test FormID is valid");

    store.get_or_create(guard_a);
    store.get_or_create(guard_b);

    assert!(store.remove(guard_a).is_some());
    assert!(store.get(guard_a).is_none());
    assert_eq!(store.len(), 1);

    store.clear();
    assert!(store.is_empty());
}
