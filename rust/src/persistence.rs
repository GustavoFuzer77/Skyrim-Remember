use crate::{MemoryStore, NpcId, NpcMemory};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;

pub const PERSISTENCE_VERSION: u32 = 1;

#[derive(Debug, PartialEq, Eq)]
pub enum PersistenceError {
    InvalidData,
    UnsupportedVersion,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct LoadStats {
    pub loaded: u32,
    pub unresolved: u32,
}

#[derive(Serialize, Deserialize)]
struct PersistedState {
    version: u32,
    memories: Vec<PersistedMemory>,
}

#[derive(Serialize, Deserialize)]
struct PersistedMemory {
    reference_form_id: u32,
    memory: NpcMemory,
}

pub fn serialize(store: &MemoryStore) -> Result<Vec<u8>, PersistenceError> {
    let mut memories: Vec<_> = store
        .iter()
        .map(|(id, memory)| PersistedMemory {
            reference_form_id: id.get(),
            memory: *memory,
        })
        .collect();
    memories.sort_unstable_by_key(|entry| entry.reference_form_id);

    bincode::serialize(&PersistedState {
        version: PERSISTENCE_VERSION,
        memories,
    })
    .map_err(|_| PersistenceError::InvalidData)
}

pub fn deserialize_with<F>(
    bytes: &[u8],
    mut resolve_form_id: F,
) -> Result<(HashMap<NpcId, NpcMemory>, LoadStats), PersistenceError>
where
    F: FnMut(u32) -> Option<u32>,
{
    let state: PersistedState =
        bincode::deserialize(bytes).map_err(|_| PersistenceError::InvalidData)?;
    if state.version != PERSISTENCE_VERSION {
        return Err(PersistenceError::UnsupportedVersion);
    }

    let mut memories = HashMap::with_capacity(state.memories.len());
    let mut stats = LoadStats::default();
    for entry in state.memories {
        if NpcId::new(entry.reference_form_id).is_none() || !entry.memory.is_valid() {
            return Err(PersistenceError::InvalidData);
        }
        let Some(resolved) = resolve_form_id(entry.reference_form_id) else {
            stats.unresolved = stats.unresolved.saturating_add(1);
            continue;
        };
        let Some(id) = NpcId::new(resolved) else {
            stats.unresolved = stats.unresolved.saturating_add(1);
            continue;
        };
        memories.insert(id, entry.memory);
        stats.loaded = stats.loaded.saturating_add(1);
    }
    Ok((memories, stats))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_is_deterministic_and_preserves_memories() {
        let mut store = MemoryStore::new();
        let a = NpcId::new(0x123).unwrap();
        let b = NpcId::new(0x456).unwrap();
        store.get_or_create_at(b, 10.0).fear = 30.0;
        store.get_or_create_at(a, 11.0).respect = 50.0;

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
        assert_eq!(loaded.get(&a).unwrap().respect, 50.0);
        assert_eq!(loaded.get(&b).unwrap().fear, 30.0);
        assert_eq!(loaded.get(&a).unwrap().last_decay_game_days, 11.0);
    }

    #[test]
    fn unresolved_forms_are_skipped_and_resolved_forms_are_remapped() {
        let mut store = MemoryStore::new();
        store.get_or_create_at(NpcId::new(10).unwrap(), 1.0).fear = 20.0;
        store.get_or_create_at(NpcId::new(20).unwrap(), 1.0).respect = 40.0;
        let bytes = serialize(&store).unwrap();

        let (loaded, stats) = deserialize_with(&bytes, |old| (old == 10).then_some(110)).unwrap();
        assert_eq!(
            stats,
            LoadStats {
                loaded: 1,
                unresolved: 1
            }
        );
        assert_eq!(loaded.get(&NpcId::new(110).unwrap()).unwrap().fear, 20.0);
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
                    fear: f32::NAN,
                    ..NpcMemory::new(1.0)
                },
            }],
        })
        .unwrap();
        assert_eq!(
            deserialize_with(&invalid, Some).unwrap_err(),
            PersistenceError::InvalidData
        );
    }
}
