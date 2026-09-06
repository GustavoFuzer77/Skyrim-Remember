//! Every number the recognition model can be tuned by, in one place.
//!
//! The algorithms in [`crate::memory`] and [`crate::appearance`] read these
//! values from the [`Tuning`] carried by the [`crate::MemoryStore`] instead of
//! embedding literals, so a future configuration file (or an ABI setter) has a
//! single structure to fill in.

use crate::appearance::AppearanceCategory;

/// Upper bound of every memory axis. Also the divisor that turns a raw
/// recognition value into the 0..=1 familiarity ratio.
pub const MAX_MEMORY_VALUE: f32 = 100.0;

/// Hard ABI cap on one appearance observation. This is a safety limit on the
/// FFI buffer rather than a tunable, so the bridge validates against it
/// directly; [`Limits::max_appearance_items`] may only lower it.
pub const MAX_APPEARANCE_ITEMS: usize = 64;

/// Default number of distinct outfits one NPC remembers.
pub const MAX_KNOWN_APPEARANCES: usize = 8;

/// Half-lives, in game hours, of each memory axis.
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

/// How much outfit evidence an NPC needs before it accepts an identity.
///
/// A stranger has to match `base_threshold`; a fully familiar NPC only has to
/// match `base_threshold - familiarity_bonus`, floored at `minimum_threshold`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct RecognitionTuning {
    pub base_threshold: f32,
    pub familiarity_bonus: f32,
    pub minimum_threshold: f32,
}

impl Default for RecognitionTuning {
    fn default() -> Self {
        Self {
            base_threshold: 0.70,
            familiarity_bonus: 0.20,
            minimum_threshold: 0.50,
        }
    }
}

impl RecognitionTuning {
    /// Similarity an outfit must reach for an NPC with this `recognition`.
    pub fn threshold(&self, recognition: f32) -> f32 {
        let familiarity = (recognition / MAX_MEMORY_VALUE).clamp(0.0, 1.0);
        (self.base_threshold - familiarity * self.familiarity_bonus)
            .clamp(self.minimum_threshold, self.base_threshold)
    }
}

/// Relative importance of each equipment category when comparing two outfits.
///
/// Only the categories present in at least one of the two outfits count, so the
/// weights are renormalized per comparison and need not sum to one.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AppearanceWeights {
    pub head: f32,
    pub body: f32,
    pub hands: f32,
    pub feet: f32,
    pub shield: f32,
    pub accessories: f32,
}

impl Default for AppearanceWeights {
    fn default() -> Self {
        Self {
            head: 0.25,
            body: 0.35,
            hands: 0.10,
            feet: 0.10,
            shield: 0.15,
            accessories: 0.05,
        }
    }
}

impl AppearanceWeights {
    pub fn get(&self, category: AppearanceCategory) -> f32 {
        match category {
            AppearanceCategory::Head => self.head,
            AppearanceCategory::Body => self.body,
            AppearanceCategory::Hands => self.hands,
            AppearanceCategory::Feet => self.feet,
            AppearanceCategory::Shield => self.shield,
            AppearanceCategory::Accessories => self.accessories,
        }
    }
}

/// Storage bounds enforced by the store itself.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Limits {
    pub max_known_appearances: usize,
    pub max_appearance_items: usize,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            max_known_appearances: MAX_KNOWN_APPEARANCES,
            max_appearance_items: MAX_APPEARANCE_ITEMS,
        }
    }
}

/// The complete parameter set of one memory store.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Tuning {
    pub decay: DecayHalfLives,
    pub recognition: RecognitionTuning,
    pub weights: AppearanceWeights,
    pub limits: Limits,
}

#[cfg(test)]
mod tests;
