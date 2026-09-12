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
    memory[Axis::Fear] = 80.0;

    memory.apply_decay(103.0, DecayHalfLives::default());

    assert!((memory[Axis::Fear] - 40.0).abs() < 0.0001);
    assert_eq!(memory.last_decay_game_days, 103.0);
}

#[test]
fn decay_ignores_time_rollback_and_deltas_are_clamped() {
    let mut memory = NpcMemory::new(20.0);
    memory[Axis::Fear] = 40.0;
    memory.apply_decay(19.0, DecayHalfLives::default());
    assert_eq!(memory[Axis::Fear], 40.0);
    assert_eq!(memory.last_decay_game_days, 20.0);

    memory.add_axis(Axis::Recognition, 200.0);
    memory.add_axis(Axis::Suspicion, -10.0);
    memory.add_axis(Axis::Fear, -100.0);
    memory.add_axis(Axis::Respect, 50.0);
    assert_eq!(memory[Axis::Recognition], 100.0);
    assert_eq!(memory[Axis::Suspicion], 0.0);
    assert_eq!(memory[Axis::Fear], 0.0);
    assert_eq!(memory[Axis::Respect], 50.0);
}

#[test]
fn register_reports_creation_only_the_first_time_and_decays_afterwards() {
    let mut store = MemoryStore::new();
    let guard = NpcId::new(123).expect("the test FormID is valid");

    assert!(store.register(guard, 100.0));
    store.get_mut(guard).expect("just created")[Axis::Fear] = 80.0;

    assert!(!store.register(guard, 103.0));
    let memory = store.get(guard).expect("still present");
    assert!((memory[Axis::Fear] - 40.0).abs() < 0.0001);
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
    let mut decay = DecayHalfLives::default();
    decay[Axis::Fear] = 24.0;
    let tuning = Tuning {
        decay,
        ..Tuning::default()
    };
    let mut store = MemoryStore::with_tuning(tuning);
    let guard = NpcId::new(123).expect("the test FormID is valid");
    store.get_or_create_at(guard, 1.0)[Axis::Fear] = 80.0;

    let memory = store.touch(guard, 2.0).expect("the memory exists");

    assert!((memory[Axis::Fear] - 40.0).abs() < 0.0001);
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

    store.get_or_create(guard)[Axis::Recognition] = 42.0;
    let memory = store.get_or_create(guard);

    assert_eq!(memory[Axis::Recognition], 42.0);
    assert_eq!(store.len(), 1);
}

#[test]
fn memories_are_individual_per_actor_reference() {
    let mut store = MemoryStore::new();
    let guard_a = NpcId::new(0x000A_1234).expect("the test FormID is valid");
    let guard_b = NpcId::new(0x000A_5678).expect("the test FormID is valid");

    store.get_or_create(guard_a)[Axis::Suspicion] = 35.0;
    store.get_or_create(guard_b)[Axis::Fear] = 20.0;

    assert_eq!(
        store.get(guard_a).map(|memory| memory[Axis::Suspicion]),
        Some(35.0)
    );
    assert_eq!(
        store.get(guard_a).map(|memory| memory[Axis::Fear]),
        Some(0.0)
    );
    assert_eq!(
        store.get(guard_b).map(|memory| memory[Axis::Suspicion]),
        Some(0.0)
    );
    assert_eq!(
        store.get(guard_b).map(|memory| memory[Axis::Fear]),
        Some(20.0)
    );
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

#[test]
fn a_reserve_slot_holds_its_value_instead_of_draining_away() {
    // Approval has no half-life configured. An axis whose tuning was forgotten
    // must keep what it has; zeroing it would erase data in silence.
    let mut memory = NpcMemory::new(1.0);
    memory[Axis::Approval] = 60.0;
    memory[Axis::Fear] = 80.0;

    memory.apply_decay(4.0, DecayHalfLives::default());

    assert_eq!(memory[Axis::Approval], 60.0);
    assert!(memory[Axis::Fear] < 80.0, "fear is tuned and must fade");
}

#[test]
fn every_named_axis_fits_inside_the_reserve() {
    assert!(Axis::ALL.len() <= AXIS_CAPACITY);
    for (slot, axis) in Axis::ALL.iter().enumerate() {
        assert_eq!(axis.index(), slot, "{} moved slot", axis.name());
    }
}

#[test]
fn axes_are_independent_and_each_one_clamps_on_its_own() {
    let mut memory = NpcMemory::new(1.0);

    memory.add_axis(Axis::Fear, 200.0);
    memory.add_axis(Axis::Loyalty, -5.0);
    memory.add_axis(Axis::Trust, 30.0);

    assert_eq!(memory[Axis::Fear], 100.0);
    assert_eq!(memory[Axis::Loyalty], 0.0);
    assert_eq!(memory[Axis::Trust], 30.0);
    assert_eq!(memory[Axis::Respect], 0.0);
}

#[test]
fn a_memory_carrying_a_follower_axis_is_still_valid() {
    // An NPC that changes profile keeps what it already had. Nothing converts.
    let mut memory = NpcMemory::new(1.0);
    memory[Axis::Fear] = 40.0;
    memory[Axis::Approval] = 20.0;

    assert!(memory.is_valid());

    memory[Axis::Approval] = f32::NAN;
    assert!(
        !memory.is_valid(),
        "a reserve slot is validated like any other"
    );
}
