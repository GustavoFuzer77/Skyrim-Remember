//! Who an NPC remembers and how strongly, independent of what it saw.
//!
//! The four memory axes and their decay live here; the appearance comparison
//! that feeds them lives in [`crate::appearance`], and every number both of
//! them read lives in [`crate::tuning`].

use crate::appearance::KnownAppearance;
use crate::tuning::{DecayHalfLives, Tuning, MAX_MEMORY_VALUE};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;

/// Identity of one actor reference in the current game session.
///
/// The C++ adapter is responsible for extracting and later resolving the
/// corresponding Skyrim reference FormID.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct NpcId(u32);

impl NpcId {
    pub fn new(reference_form_id: u32) -> Option<Self> {
        (reference_form_id != 0).then_some(Self(reference_form_id))
    }

    pub fn get(self) -> u32 {
        self.0
    }
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
pub struct NpcMemory {
    pub recognition: f32,
    pub suspicion: f32,
    pub fear: f32,
    pub respect: f32,
    pub last_decay_game_days: f64,
}

impl NpcMemory {
    pub fn new(game_days: f64) -> Self {
        Self {
            last_decay_game_days: game_days,
            ..Self::default()
        }
    }

    pub fn apply_decay(&mut self, current_game_days: f64, half_lives: DecayHalfLives) {
        if !current_game_days.is_finite() || current_game_days <= self.last_decay_game_days {
            return;
        }

        let elapsed_hours = (current_game_days - self.last_decay_game_days) * 24.0;
        self.recognition = decay(
            self.recognition,
            elapsed_hours,
            half_lives.recognition_hours,
        );
        self.suspicion = decay(self.suspicion, elapsed_hours, half_lives.suspicion_hours);
        self.fear = decay(self.fear, elapsed_hours, half_lives.fear_hours);
        self.respect = decay(self.respect, elapsed_hours, half_lives.respect_hours);
        self.last_decay_game_days = current_game_days;
    }

    pub fn add(&mut self, recognition: f32, suspicion: f32, fear: f32, respect: f32) {
        self.recognition = bounded_add(self.recognition, recognition);
        self.suspicion = bounded_add(self.suspicion, suspicion);
        self.fear = bounded_add(self.fear, fear);
        self.respect = bounded_add(self.respect, respect);
    }

    pub fn is_valid(&self) -> bool {
        [self.recognition, self.suspicion, self.fear, self.respect]
            .into_iter()
            .all(|value| value.is_finite() && (0.0..=MAX_MEMORY_VALUE).contains(&value))
            && self.last_decay_game_days.is_finite()
            && self.last_decay_game_days >= 0.0
    }
}

fn decay(value: f32, elapsed_hours: f64, half_life_hours: f64) -> f32 {
    if value <= 0.0 || !half_life_hours.is_finite() || half_life_hours <= 0.0 {
        return 0.0;
    }
    (f64::from(value) * 0.5_f64.powf(elapsed_hours / half_life_hours)) as f32
}

fn bounded_add(value: f32, delta: f32) -> f32 {
    (value + delta).clamp(0.0, MAX_MEMORY_VALUE)
}

/// Every NPC memory of one session, plus the parameters they are evaluated by.
#[derive(Debug, Default)]
pub struct MemoryStore {
    pub(crate) memories: HashMap<NpcId, NpcMemory>,
    pub(crate) appearances: HashMap<NpcId, Vec<KnownAppearance>>,
    pub(crate) tuning: Tuning,
}

impl MemoryStore {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn with_tuning(tuning: Tuning) -> Self {
        Self {
            tuning,
            ..Self::default()
        }
    }

    pub fn tuning(&self) -> &Tuning {
        &self.tuning
    }

    pub fn len(&self) -> usize {
        self.memories.len()
    }

    pub fn is_empty(&self) -> bool {
        self.memories.is_empty()
    }

    pub fn get(&self, npc_id: NpcId) -> Option<&NpcMemory> {
        self.memories.get(&npc_id)
    }

    pub fn get_mut(&mut self, npc_id: NpcId) -> Option<&mut NpcMemory> {
        self.memories.get_mut(&npc_id)
    }

    pub fn get_or_create(&mut self, npc_id: NpcId) -> &mut NpcMemory {
        self.memories.entry(npc_id).or_default()
    }

    pub fn get_or_create_at(&mut self, npc_id: NpcId, game_days: f64) -> &mut NpcMemory {
        self.memories
            .entry(npc_id)
            .or_insert_with(|| NpcMemory::new(game_days))
    }

    /// Brings an existing memory forward to `game_days`. Creates nothing.
    pub fn touch(&mut self, npc_id: NpcId, game_days: f64) -> Option<&mut NpcMemory> {
        let half_lives = self.tuning.decay;
        let memory = self.memories.get_mut(&npc_id)?;
        memory.apply_decay(game_days, half_lives);
        Some(memory)
    }

    /// Brings a memory forward to `game_days`, creating it when absent.
    pub fn touch_or_create(&mut self, npc_id: NpcId, game_days: f64) -> &mut NpcMemory {
        let half_lives = self.tuning.decay;
        let memory = self
            .memories
            .entry(npc_id)
            .or_insert_with(|| NpcMemory::new(game_days));
        memory.apply_decay(game_days, half_lives);
        memory
    }

    /// Registers a reference without changing its values. Reports whether the
    /// memory had to be created.
    pub fn register(&mut self, npc_id: NpcId, game_days: f64) -> bool {
        let created = !self.memories.contains_key(&npc_id);
        self.touch_or_create(npc_id, game_days);
        created
    }

    /// Swaps in restored data while keeping this store's tuning.
    pub fn replace(&mut self, other: Self) {
        self.memories = other.memories;
        self.appearances = other.appearances;
    }

    pub fn iter(&self) -> impl Iterator<Item = (&NpcId, &NpcMemory)> {
        self.memories.iter()
    }

    pub fn known_appearances(&self, npc_id: NpcId) -> &[KnownAppearance] {
        self.appearances
            .get(&npc_id)
            .map(Vec::as_slice)
            .unwrap_or(&[])
    }

    pub fn restore_entry(
        &mut self,
        npc_id: NpcId,
        memory: NpcMemory,
        appearances: Vec<KnownAppearance>,
    ) {
        self.memories.insert(npc_id, memory);
        if !appearances.is_empty() {
            self.appearances.insert(npc_id, appearances);
        }
    }

    pub fn remove(&mut self, npc_id: NpcId) -> Option<NpcMemory> {
        self.appearances.remove(&npc_id);
        self.memories.remove(&npc_id)
    }

    pub fn clear(&mut self) {
        self.memories.clear();
        self.appearances.clear();
    }
}

#[cfg(test)]
mod tests;
