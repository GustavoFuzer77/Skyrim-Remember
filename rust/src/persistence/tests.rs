use super::*;
use crate::memory::AXIS_CAPACITY;

/// Every test here restores with the default configuration.
fn deserialize_with<F>(
    bytes: &[u8],
    resolve_form_id: F,
) -> Result<(MemoryStore, LoadStats), PersistenceError>
where
    F: FnMut(u32) -> Option<u32>,
{
    deserialize_with_tuning(bytes, Tuning::default(), resolve_form_id)
}

/// One pre-v4 memory, the way v1, v2 and v3 saves stored it.
fn legacy(fear: f32, last_decay_game_days: f64) -> NpcMemoryLegacy {
    NpcMemoryLegacy {
        recognition: 0.0,
        suspicion: 0.0,
        fear,
        respect: 0.0,
        last_decay_game_days,
    }
}

#[test]
fn round_trip_is_deterministic_and_preserves_memories_and_appearance() {
    let mut store = MemoryStore::new();
    let a = NpcId::new(0x123).unwrap();
    let b = NpcId::new(0x456).unwrap();
    store.get_or_create_at(b, 10.0)[Axis::Fear] = 30.0;
    store.get_or_create_at(a, 11.0)[Axis::Respect] = 50.0;
    store.observe_appearance(
        a,
        vec![AppearanceItem {
            form_id: 0x1000,
            slot_mask: 1 << 2,
        }],
        true,
        false,
        11.0,
    );

    let first = serialize(&store).unwrap();
    let second = serialize(&store).unwrap();
    assert_eq!(first, second);

    let (loaded, stats) = deserialize_with(&first, Some).unwrap();
    assert_eq!(
        stats,
        LoadStats {
            loaded: 2,
            unresolved: 0
        }
    );
    assert_eq!(loaded.get(a).unwrap()[Axis::Respect], 50.0);
    assert_eq!(loaded.get(b).unwrap()[Axis::Fear], 30.0);
    assert_eq!(loaded.get(a).unwrap().last_decay_game_days, 11.0);
    assert_eq!(
        loaded.get(a).unwrap().last_recognition_game_days,
        Some(11.0)
    );
    assert_eq!(loaded.known_appearances(a)[0].items[0].form_id, 0x1000);
}

#[test]
fn npc_and_equipment_forms_are_remapped_and_missing_items_are_dropped() {
    let mut store = MemoryStore::new();
    let id = NpcId::new(10).unwrap();
    store.get_or_create_at(id, 1.0)[Axis::Fear] = 20.0;
    store.observe_appearance(
        id,
        vec![
            AppearanceItem {
                form_id: 20,
                slot_mask: 1 << 2,
            },
            AppearanceItem {
                form_id: 30,
                slot_mask: 1 << 3,
            },
        ],
        false,
        false,
        1.0,
    );
    let bytes = serialize(&store).unwrap();

    let (loaded, stats) = deserialize_with(&bytes, |old| match old {
        10 => Some(110),
        20 => Some(120),
        _ => None,
    })
    .unwrap();
    let loaded_id = NpcId::new(110).unwrap();
    assert_eq!(
        stats,
        LoadStats {
            loaded: 1,
            unresolved: 1
        }
    );
    assert_eq!(loaded.get(loaded_id).unwrap()[Axis::Fear], 20.0);
    assert_eq!(loaded.known_appearances(loaded_id)[0].items.len(), 1);
    assert_eq!(loaded.known_appearances(loaded_id)[0].items[0].form_id, 120);
}

#[test]
fn corrupted_data_is_rejected() {
    assert_eq!(
        deserialize_with(&[1, 2, 3], Some).unwrap_err(),
        PersistenceError::InvalidData
    );
}

#[test]
fn unsupported_versions_and_invalid_values_are_rejected() {
    let unsupported = bincode::serialize(&PersistedState {
        version: PERSISTENCE_VERSION + 1,
        memories: Vec::new(),
    })
    .unwrap();
    assert_eq!(
        deserialize_with(&unsupported, Some).unwrap_err(),
        PersistenceError::UnsupportedVersion
    );

    let invalid = bincode::serialize(&PersistedState {
        version: PERSISTENCE_VERSION,
        memories: vec![PersistedMemory {
            reference_form_id: 10,
            memory: NpcMemory {
                axes: {
                    let mut a = [0.0; AXIS_CAPACITY];
                    a[Axis::Fear.index()] = f32::NAN;
                    a
                },
                ..NpcMemory::new(1.0)
            },
            appearances: Vec::new(),
        }],
    })
    .unwrap();
    assert_eq!(
        deserialize_with(&invalid, Some).unwrap_err(),
        PersistenceError::InvalidData
    );
}

#[test]
fn version_one_data_migrates_without_appearances() {
    let old = bincode::serialize(&PersistedStateV1 {
        version: 1,
        memories: vec![PersistedMemoryV1 {
            reference_form_id: 10,
            memory: legacy(25.0, 3.0),
        }],
    })
    .unwrap();

    let (loaded, stats) = deserialize_with(&old, Some).unwrap();
    let id = NpcId::new(10).unwrap();
    assert_eq!(stats.loaded, 1);
    assert_eq!(loaded.get(id).unwrap()[Axis::Fear], 25.0);
    assert!(loaded.known_appearances(id).is_empty());
}

#[test]
fn structurally_compatible_old_states_dispatch_by_their_version() {
    let version_one = bincode::serialize(&PersistedStateV1 {
        version: 1,
        memories: Vec::new(),
    })
    .unwrap();
    let version_two = bincode::serialize(&PersistedStateV2 {
        version: 2,
        memories: vec![PersistedMemoryV2 {
            reference_form_id: 10,
            memory: legacy(0.0, 1.0),
            appearances: Vec::new(),
        }],
    })
    .unwrap();

    assert!(deserialize_with(&version_one, Some).unwrap().0.is_empty());
    let (migrated, stats) = deserialize_with(&version_two, Some).unwrap();
    assert_eq!(stats.loaded, 1);
    assert!(migrated.get(NpcId::new(10).unwrap()).is_some());
}

#[test]
fn version_two_data_migrates_visible_faces_as_known_identity() {
    let old = bincode::serialize(&PersistedStateV2 {
        version: 2,
        memories: vec![PersistedMemoryV2 {
            reference_form_id: 10,
            memory: legacy(0.0, 3.0),
            appearances: vec![
                KnownAppearanceV2 {
                    items: vec![AppearanceItem {
                        form_id: 20,
                        slot_mask: 4,
                    }],
                    face_visible: true,
                    last_seen_game_days: 3.0,
                },
                KnownAppearanceV2 {
                    items: vec![AppearanceItem {
                        form_id: 30,
                        slot_mask: 4,
                    }],
                    face_visible: false,
                    last_seen_game_days: 3.0,
                },
            ],
        }],
    })
    .unwrap();

    let (loaded, _) = deserialize_with(&old, Some).unwrap();
    let appearances = loaded.known_appearances(NpcId::new(10).unwrap());
    assert!(appearances[0].identity_known);
    assert!(!appearances[1].identity_known);
}

#[test]
fn version_three_data_migrates_the_four_fixed_axes_into_their_slots() {
    let old = bincode::serialize(&PersistedStateV3 {
        version: 3,
        memories: vec![PersistedMemoryV3 {
            reference_form_id: 10,
            memory: NpcMemoryLegacy {
                recognition: 11.0,
                suspicion: 22.0,
                fear: 33.0,
                respect: 44.0,
                last_decay_game_days: 5.0,
            },
            appearances: vec![KnownAppearance {
                items: vec![AppearanceItem {
                    form_id: 20,
                    slot_mask: 1 << 2,
                }],
                face_visible: true,
                identity_known: true,
                last_seen_game_days: 5.0,
            }],
        }],
    })
    .unwrap();

    let (loaded, stats) = deserialize_with(&old, Some).unwrap();

    let id = NpcId::new(10).unwrap();
    let memory = loaded.get(id).expect("the entry survived the migration");
    assert_eq!(stats.loaded, 1);
    assert_eq!(memory[Axis::Recognition], 11.0);
    assert_eq!(memory[Axis::Suspicion], 22.0);
    assert_eq!(memory[Axis::Fear], 33.0);
    assert_eq!(memory[Axis::Respect], 44.0);
    assert_eq!(memory.last_decay_game_days, 5.0);
    // Axes named after v3 was written load at zero, which is what "this NPC
    // never felt that" means.
    assert_eq!(memory[Axis::Approval], 0.0);
    assert_eq!(memory[Axis::Loyalty], 0.0);
    assert_eq!(loaded.known_appearances(id).len(), 1);
    assert!(loaded.known_appearances(id)[0].identity_known);
}

#[test]
fn version_four_data_starts_without_a_familiarity_cooldown() {
    let old = bincode::serialize(&PersistedStateV4 {
        version: 4,
        memories: vec![PersistedMemoryV4 {
            reference_form_id: 10,
            memory: NpcMemoryV4 {
                axes: [0.0; AXIS_CAPACITY],
                last_decay_game_days: 5.0,
            },
            appearances: Vec::new(),
        }],
    })
    .unwrap();

    let (loaded, stats) = deserialize_with(&old, Some).unwrap();
    let memory = loaded.get(NpcId::new(10).unwrap()).unwrap();

    assert_eq!(stats.loaded, 1);
    assert_eq!(memory.last_recognition_game_days, None);
}

#[test]
fn every_earlier_payload_version_still_loads() {
    for (bytes, label) in [
        (
            bincode::serialize(&PersistedStateV1 {
                version: 1,
                memories: vec![PersistedMemoryV1 {
                    reference_form_id: 10,
                    memory: legacy(25.0, 3.0),
                }],
            })
            .unwrap(),
            "v1",
        ),
        (
            bincode::serialize(&PersistedStateV2 {
                version: 2,
                memories: vec![PersistedMemoryV2 {
                    reference_form_id: 10,
                    memory: legacy(25.0, 3.0),
                    appearances: Vec::new(),
                }],
            })
            .unwrap(),
            "v2",
        ),
        (
            bincode::serialize(&PersistedStateV3 {
                version: 3,
                memories: vec![PersistedMemoryV3 {
                    reference_form_id: 10,
                    memory: legacy(25.0, 3.0),
                    appearances: Vec::new(),
                }],
            })
            .unwrap(),
            "v3",
        ),
        (
            bincode::serialize(&PersistedStateV4 {
                version: 4,
                memories: vec![PersistedMemoryV4 {
                    reference_form_id: 10,
                    memory: NpcMemoryV4 {
                        axes: {
                            let mut axes = [0.0; AXIS_CAPACITY];
                            axes[Axis::Fear.index()] = 25.0;
                            axes
                        },
                        last_decay_game_days: 3.0,
                    },
                    appearances: Vec::new(),
                }],
            })
            .unwrap(),
            "v4",
        ),
    ] {
        let (loaded, stats) = deserialize_with(&bytes, Some)
            .unwrap_or_else(|error| panic!("{label} failed to load: {error:?}"));
        let memory = loaded
            .get(NpcId::new(10).unwrap())
            .unwrap_or_else(|| panic!("{label} lost its entry"));
        assert_eq!(stats.loaded, 1, "{label}");
        assert_eq!(memory[Axis::Fear], 25.0, "{label}");
    }
}
