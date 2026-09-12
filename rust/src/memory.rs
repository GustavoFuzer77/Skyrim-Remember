//! Who an NPC remembers and how strongly, independent of what it saw.
//!
//! The axes and their decay live here; the appearance comparison that feeds
//! them lives in [`crate::appearance`], and every number both of them read
//! lives in [`crate::tuning`].

use crate::appearance::{normalize_items, KnownAppearance};
use crate::tuning::{DecayHalfLives, Tuning, MAX_MEMORY_VALUE};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::ops::{Index, IndexMut};

/// How many axis slots every NPC memory carries.
///
/// Deliberately larger than the number of axes actually named below. The spare
/// slots are what make the set open: adding an [`Axis`] variant that fits in
/// the reserve costs one line, and changes neither the ABI struct sizes nor the
/// persisted payload version. An older save simply loads the new axis at zero,
/// which is the right value for "this NPC never felt that".
pub const AXIS_CAPACITY: usize = 16;

/// One dimension of what an NPC feels about the Player.
///
/// Storage is uniform: every NPC carries every slot. Which axes actually get
/// written is a matter of behaviour — a guard's temperament touches fear and
/// respect, a follower's would touch approval and loyalty — never of type. That
/// is what lets an NPC change profile (a Whiterun housecarl becoming a
/// follower, say) without converting or losing anything: the axes already
/// written stay written, and the new ones start at zero.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Axis {
    /// How well the NPC knows the Player by sight. Read by the appearance
    /// system, so it is the one axis every profile shares.
    Recognition = 0,
    Suspicion = 1,
    Fear = 2,
    Respect = 3,
    /// Reserved for the Follower profile. Nothing writes these yet; they exist
    /// so that the shape of the memory does not have to change when something
    /// does.
    Approval = 4,
    Loyalty = 5,
    Resentment = 6,
    Trust = 7,
}

impl Axis {
    /// Every named axis, in slot order. The remaining slots up to
    /// [`AXIS_CAPACITY`] are reserve and always read as zero.
    pub const ALL: [Axis; 8] = [
        Axis::Recognition,
        Axis::Suspicion,
        Axis::Fear,
        Axis::Respect,
        Axis::Approval,
        Axis::Loyalty,
        Axis::Resentment,
        Axis::Trust,
    ];

    pub const fn index(self) -> usize {
        self as usize
    }

    pub const fn name(self) -> &'static str {
        match self {
            Axis::Recognition => "recognition",
            Axis::Suspicion => "suspicion",
            Axis::Fear => "fear",
            Axis::Respect => "respect",
            Axis::Approval => "approval",
            Axis::Loyalty => "loyalty",
            Axis::Resentment => "resentment",
            Axis::Trust => "trust",
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

/// What one NPC feels about the Player, and when that was last brought
/// up to date.
#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
pub struct NpcMemory {
    pub axes: [f32; AXIS_CAPACITY],
    pub last_decay_game_days: f64,
}

impl Index<Axis> for NpcMemory {
    type Output = f32;

    fn index(&self, axis: Axis) -> &f32 {
        &self.axes[axis.index()]
    }
}

impl IndexMut<Axis> for NpcMemory {
    fn index_mut(&mut self, axis: Axis) -> &mut f32 {
        &mut self.axes[axis.index()]
    }
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
        for (value, half_life) in self.axes.iter_mut().zip(half_lives.hours) {
            *value = decay(*value, elapsed_hours, half_life);
        }
        self.last_decay_game_days = current_game_days;
    }

    /// Adds one delta per slot, clamping every axis to `0..=MAX_MEMORY_VALUE`.
    pub fn add(&mut self, deltas: &[f32; AXIS_CAPACITY]) {
        for (value, delta) in self.axes.iter_mut().zip(deltas) {
            *value = bounded_add(*value, *delta);
        }
    }

    pub fn add_axis(&mut self, axis: Axis, delta: f32) {
        self[axis] = bounded_add(self[axis], delta);
    }

    pub fn is_valid(&self) -> bool {
        self.axes
            .iter()
            .all(|value| value.is_finite() && (0.0..=MAX_MEMORY_VALUE).contains(value))
            && self.last_decay_game_days.is_finite()
            && self.last_decay_game_days >= 0.0
    }
}

fn decay(value: f32, elapsed_hours: f64, half_life_hours: f64) -> f32 {
    // A slot with no configured half-life does not fade. Zeroing it instead
    // would silently erase a newly added axis whose tuning was forgotten, which
    // is exactly the mistake the reserve slots invite.
    if value <= 0.0 || !half_life_hours.is_finite() || half_life_hours <= 0.0 {
        return value.max(0.0);
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

    /// Inserts a restored entry, normalizing its outfits on the way in.
    ///
    /// Normalizing here rather than trusting the caller is what lets an outfit
    /// saved under older rules — technical slots included, slots unmerged —
    /// compare equal to the same outfit captured today.
    pub fn restore_entry(
        &mut self,
        npc_id: NpcId,
        memory: NpcMemory,
        mut appearances: Vec<KnownAppearance>,
    ) {
        self.memories.insert(npc_id, memory);
        for appearance in &mut appearances {
            normalize_items(&mut appearance.items);
        }
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
