use serde::{Deserialize, Serialize};
use std::collections::HashMap;

pub const MAX_MEMORY_VALUE: f32 = 100.0;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct DecayHalfLives {
    pub recognition_hours: f64,
    pub suspicion_hours: f64,
    pub fear_hours: f64,
    pub respect_hours: f64,
}

impl Default for DecayHalfLives {
    fn default() -> Self {
        Self {
            recognition_hours: 24.0 * 30.0,
            suspicion_hours: 48.0,
            fear_hours: 72.0,
            respect_hours: 24.0 * 30.0,
        }
    }
}

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

#[derive(Debug, Default)]
pub struct MemoryStore {
    memories: HashMap<NpcId, NpcMemory>,
}

impl MemoryStore {
    pub fn new() -> Self {
        Self::default()
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

    pub fn replace(&mut self, memories: HashMap<NpcId, NpcMemory>) {
        self.memories = memories;
    }

    pub fn iter(&self) -> impl Iterator<Item = (&NpcId, &NpcMemory)> {
        self.memories.iter()
    }

    pub fn remove(&mut self, npc_id: NpcId) -> Option<NpcMemory> {
        self.memories.remove(&npc_id)
    }

    pub fn clear(&mut self) {
        self.memories.clear();
    }
}
