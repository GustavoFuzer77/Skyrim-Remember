use crate::appearance::{appearance_is_valid, normalize_items};
use crate::memory::{MemoryStore, NpcId, NpcMemory};
use crate::tuning::Tuning;
use crate::{AppearanceItem, KnownAppearance};
use serde::{Deserialize, Serialize};

pub const PERSISTENCE_VERSION: u32 = 3;

#[derive(Debug, PartialEq, Eq)]
pub enum PersistenceError {
    InvalidData,
    UnsupportedVersion,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct LoadStats {
    pub loaded: u32,
    /// Number of unresolved NPC references and equipped item Forms.
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
    appearances: Vec<KnownAppearance>,
}

#[derive(Serialize, Deserialize)]
struct PersistedStateV2 {
    version: u32,
    memories: Vec<PersistedMemoryV2>,
}

#[derive(Serialize, Deserialize)]
struct PersistedMemoryV2 {
    reference_form_id: u32,
    memory: NpcMemory,
    appearances: Vec<KnownAppearanceV2>,
}

#[derive(Serialize, Deserialize)]
struct KnownAppearanceV2 {
    items: Vec<AppearanceItem>,
    face_visible: bool,
    last_seen_game_days: f64,
}

#[derive(Serialize, Deserialize)]
struct PersistedStateV1 {
    version: u32,
    memories: Vec<PersistedMemoryV1>,
}

#[derive(Serialize, Deserialize)]
struct PersistedMemoryV1 {
    reference_form_id: u32,
    memory: NpcMemory,
}

pub fn serialize(store: &MemoryStore) -> Result<Vec<u8>, PersistenceError> {
    let mut memories: Vec<_> = store
        .iter()
        .map(|(id, memory)| PersistedMemory {
            reference_form_id: id.get(),
            memory: *memory,
            appearances: store.known_appearances(*id).to_vec(),
        })
        .collect();
    memories.sort_unstable_by_key(|entry| entry.reference_form_id);

    bincode::serialize(&PersistedState {
        version: PERSISTENCE_VERSION,
        memories,
    })
    .map_err(|_| PersistenceError::InvalidData)
}

/// Restores a store that will be evaluated with `tuning`, validating the saved
/// data against that same configuration rather than against the defaults.
pub fn deserialize_with_tuning<F>(
    bytes: &[u8],
    tuning: Tuning,
    mut resolve_form_id: F,
) -> Result<(MemoryStore, LoadStats), PersistenceError>
where
    F: FnMut(u32) -> Option<u32>,
{
    let state = decode_state(bytes)?;
    let mut store = MemoryStore::with_tuning(tuning);
    let limits = tuning.limits;
    let mut stats = LoadStats::default();

    for entry in state.memories {
        if NpcId::new(entry.reference_form_id).is_none()
            || !entry.memory.is_valid()
            || entry.appearances.len() > limits.max_known_appearances
            || !entry
                .appearances
                .iter()
                .all(|appearance| appearance_is_valid(appearance, limits.max_appearance_items))
        {
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

        let mut appearances = Vec::with_capacity(entry.appearances.len());
        for appearance in entry.appearances {
            let mut items = Vec::with_capacity(appearance.items.len());
            for item in appearance.items {
                if let Some(resolved_item) = resolve_form_id(item.form_id) {
                    items.push(AppearanceItem {
                        form_id: resolved_item,
                        slot_mask: item.slot_mask,
                    });
                } else {
                    stats.unresolved = stats.unresolved.saturating_add(1);
                }
            }
            normalize_items(&mut items);
            if appearance.face_visible || !items.is_empty() {
                appearances.push(KnownAppearance {
                    items,
                    face_visible: appearance.face_visible,
                    identity_known: appearance.identity_known,
                    last_seen_game_days: appearance.last_seen_game_days,
                });
            }
        }

        store.restore_entry(id, entry.memory, appearances);
        stats.loaded = stats.loaded.saturating_add(1);
    }
    Ok((store, stats))
}

fn decode_state(bytes: &[u8]) -> Result<PersistedState, PersistenceError> {
    let version_bytes: [u8; 4] = bytes
        .get(..4)
        .and_then(|prefix| prefix.try_into().ok())
        .ok_or(PersistenceError::InvalidData)?;
    match u32::from_le_bytes(version_bytes) {
        PERSISTENCE_VERSION => {
            bincode::deserialize(bytes).map_err(|_| PersistenceError::InvalidData)
        }
        2 => {
            let old: PersistedStateV2 =
                bincode::deserialize(bytes).map_err(|_| PersistenceError::InvalidData)?;
            Ok(PersistedState {
                version: PERSISTENCE_VERSION,
                memories: old
                    .memories
                    .into_iter()
                    .map(|entry| PersistedMemory {
                        reference_form_id: entry.reference_form_id,
                        memory: entry.memory,
                        appearances: entry
                            .appearances
                            .into_iter()
                            .map(|appearance| KnownAppearance {
                                items: appearance.items,
                                face_visible: appearance.face_visible,
                                identity_known: appearance.face_visible,
                                last_seen_game_days: appearance.last_seen_game_days,
                            })
                            .collect(),
                    })
                    .collect(),
            })
        }
        1 => {
            let old: PersistedStateV1 =
                bincode::deserialize(bytes).map_err(|_| PersistenceError::InvalidData)?;
            Ok(PersistedState {
                version: PERSISTENCE_VERSION,
                memories: old
                    .memories
                    .into_iter()
                    .map(|entry| PersistedMemory {
                        reference_form_id: entry.reference_form_id,
                        memory: entry.memory,
                        appearances: Vec::new(),
                    })
                    .collect(),
            })
        }
        _ => Err(PersistenceError::UnsupportedVersion),
    }
}

#[cfg(test)]
mod tests;
