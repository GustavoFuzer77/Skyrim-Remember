//! What the Player looks like, and how close that is to what an NPC remembers.
//!
//! This module owns the whole recognition algorithm: the slot taxonomy, the
//! weighted outfit comparison, and the rules that decide when a new outfit may
//! be attached to a known identity. [`crate::memory`] only stores the result.

use crate::memory::{Axis, MemoryStore, NpcId};
use crate::tuning::AppearanceWeights;
use serde::{Deserialize, Serialize};

const HEAD_SLOTS: u32 = (1 << 0) | (1 << 1) | (1 << 12) | (1 << 13) | (1 << 14) | (1 << 25);
const BODY_SLOTS: u32 =
    (1 << 2) | (1 << 16) | (1 << 17) | (1 << 19) | (1 << 22) | (1 << 26) | (1 << 27);
const HAND_SLOTS: u32 = (1 << 3) | (1 << 4) | (1 << 28) | (1 << 29);
const FEET_SLOTS: u32 = (1 << 7) | (1 << 8) | (1 << 23) | (1 << 24);
const SHIELD_SLOTS: u32 = 1 << 9;
const ACCESSORY_SLOTS: u32 =
    (1 << 5) | (1 << 6) | (1 << 10) | (1 << 11) | (1 << 15) | (1 << 18) | (1 << 30) | (1 << 31);

/// Biped slots that carry no visual meaning: bit 20 (`kDecapitateHead`) and
/// bit 21 (`kDecapitate`), which the game uses for decapitation geometry.
///
/// They are stripped from every item, on capture and on load, so that an outfit
/// saved before this rule still compares equal to the same outfit captured
/// today. A piece is dropped only when nothing but technical bits remains;
/// a piece that mixes visual and technical slots keeps its visual bits.
pub const TECHNICAL_SLOTS: u32 = (1 << 20) | (1 << 21);

/// A group of biped slots that reads as one visual feature at a distance.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum AppearanceCategory {
    Head,
    Body,
    Hands,
    Feet,
    Shield,
    Accessories,
}

/// Every category paired with the biped slot bits that belong to it.
pub const APPEARANCE_CATEGORIES: [(AppearanceCategory, u32); 6] = [
    (AppearanceCategory::Head, HEAD_SLOTS),
    (AppearanceCategory::Body, BODY_SLOTS),
    (AppearanceCategory::Hands, HAND_SLOTS),
    (AppearanceCategory::Feet, FEET_SLOTS),
    (AppearanceCategory::Shield, SHIELD_SLOTS),
    (AppearanceCategory::Accessories, ACCESSORY_SLOTS),
];

/// One worn Form and the biped slots it occupies. Layout is part of the ABI.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct AppearanceItem {
    pub form_id: u32,
    pub slot_mask: u32,
}

/// An outfit one NPC has already seen, and whether it knows whose it is.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct KnownAppearance {
    pub items: Vec<AppearanceItem>,
    pub face_visible: bool,
    pub identity_known: bool,
    pub last_seen_game_days: f64,
}

/// The verdict of comparing a current appearance against one NPC's memory.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AppearanceObservation {
    pub outfit_similarity: f32,
    pub identity_confidence: f32,
    pub recognition_threshold: f32,
    pub appearance_matched: bool,
    pub recognized: bool,
    pub linked_by_continuity: bool,
    pub learned: bool,
    pub item_count: u32,
    pub known_appearances: u32,
}

impl MemoryStore {
    /// Evaluates an appearance and then learns from it.
    ///
    /// The first outfit an NPC sees is always learned. Later novel outfits are
    /// learned only when the NPC can attach them to a known identity, through a
    /// visible face, a matching outfit, or host-supplied visual continuity.
    pub fn observe_appearance(
        &mut self,
        npc_id: NpcId,
        mut items: Vec<AppearanceItem>,
        face_visible: bool,
        visual_continuity: bool,
        game_days: f64,
    ) -> Option<AppearanceObservation> {
        normalize_items(&mut items);
        if items.len() > self.tuning.limits.max_appearance_items {
            return None;
        }
        let mut observation =
            self.evaluate_normalized_appearance(npc_id, &items, face_visible, visual_continuity)?;
        let capacity = self.tuning.limits.max_known_appearances;
        let weights = self.tuning.weights;
        let refresh_interval = self.tuning.observation.refresh_interval_game_days;
        let recognition_interval = self.tuning.observation.recognition_interval_game_days;
        let recognition_gain = self.tuning.observation.recognition_gain;
        let reinforce_recognition = face_visible
            && self
                .memories
                .get(&npc_id)?
                .recognition_is_due(game_days, recognition_interval);
        let known = self.appearances.entry(npc_id).or_default();

        let learned = if known.is_empty() {
            known.push(KnownAppearance {
                items,
                face_visible,
                identity_known: observation.recognized,
                last_seen_game_days: game_days,
            });
            true
        } else {
            // Seeing the Player's face can link a previously anonymous outfit
            // to the Player, but only when the clothing evidence is strong
            // enough.
            if face_visible {
                for previous in known
                    .iter_mut()
                    .filter(|appearance| !appearance.identity_known)
                {
                    if outfit_similarity(&previous.items, &items, &weights)
                        >= observation.recognition_threshold
                    {
                        previous.identity_known = true;
                    }
                }
            }
            let exact_index = known.iter().position(|previous| {
                previous.face_visible == face_visible && previous.items == items
            });
            if observation.recognized {
                if let Some(index) = exact_index {
                    // Seeing a known outfit again is not news. Only move the
                    // freshness stamp once the refresh interval has really
                    // passed, so repeated polling is idempotent.
                    let previous = &mut known[index];
                    if game_days - previous.last_seen_game_days >= refresh_interval {
                        previous.last_seen_game_days = game_days;
                    }
                    previous.identity_known = true;
                    false
                } else {
                    if known.len() >= capacity {
                        let oldest = known
                            .iter()
                            .enumerate()
                            .min_by(|(_, left), (_, right)| {
                                left.last_seen_game_days
                                    .total_cmp(&right.last_seen_game_days)
                            })
                            .map(|(index, _)| index)
                            .unwrap_or(0);
                        known.remove(oldest);
                    }
                    known.push(KnownAppearance {
                        items,
                        face_visible,
                        identity_known: true,
                        last_seen_game_days: game_days,
                    });
                    true
                }
            } else {
                false
            }
        };

        observation.learned = learned;
        observation.known_appearances = u32::try_from(known.len()).unwrap_or(u32::MAX);
        if reinforce_recognition {
            self.memories
                .get_mut(&npc_id)?
                .reinforce_recognition(game_days, recognition_gain);
        }
        Some(observation)
    }

    /// Evaluates recognition without learning, refreshing, or creating anything.
    pub fn evaluate_appearance(
        &self,
        npc_id: NpcId,
        mut items: Vec<AppearanceItem>,
        face_visible: bool,
        visual_continuity: bool,
    ) -> Option<AppearanceObservation> {
        normalize_items(&mut items);
        if items.len() > self.tuning.limits.max_appearance_items {
            return None;
        }
        self.evaluate_normalized_appearance(npc_id, &items, face_visible, visual_continuity)
    }

    fn evaluate_normalized_appearance(
        &self,
        npc_id: NpcId,
        items: &[AppearanceItem],
        face_visible: bool,
        visual_continuity: bool,
    ) -> Option<AppearanceObservation> {
        let memory = self.memories.get(&npc_id)?;
        let threshold = self.tuning.recognition.threshold(memory[Axis::Recognition]);
        let known = self.known_appearances(npc_id);
        let mut best_similarity = 0.0_f32;
        let mut best_known_identity_similarity = 0.0_f32;
        for previous in known {
            let similarity = outfit_similarity(&previous.items, items, &self.tuning.weights);
            best_similarity = best_similarity.max(similarity);
            if previous.identity_known {
                best_known_identity_similarity = best_known_identity_similarity.max(similarity);
            }
        }
        let identity_confidence = if face_visible || visual_continuity {
            1.0
        } else {
            best_known_identity_similarity
        };
        Some(AppearanceObservation {
            outfit_similarity: best_similarity,
            identity_confidence,
            recognition_threshold: threshold,
            appearance_matched: !known.is_empty() && best_similarity >= threshold,
            recognized: identity_confidence >= threshold,
            linked_by_continuity: visual_continuity
                && !face_visible
                && best_known_identity_similarity < threshold,
            learned: false,
            item_count: u32::try_from(items.len()).unwrap_or(u32::MAX),
            known_appearances: u32::try_from(known.len()).unwrap_or(u32::MAX),
        })
    }
}

/// Rejects appearances that could not have been produced by the game.
pub fn appearance_is_valid(appearance: &KnownAppearance, max_items: usize) -> bool {
    appearance.items.len() <= max_items
        && appearance
            .items
            .iter()
            .all(|item| item.form_id != 0 && item.slot_mask != 0)
        && appearance.last_seen_game_days.is_finite()
        && appearance.last_seen_game_days >= 0.0
}

/// Strips technical slots, drops empty entries, sorts by FormID, and merges the
/// slots of one Form worn in several slots, so that two equal outfits compare
/// bit-for-bit equal.
pub(crate) fn normalize_items(items: &mut Vec<AppearanceItem>) {
    items.retain_mut(|item| {
        item.slot_mask &= !TECHNICAL_SLOTS;
        item.form_id != 0 && item.slot_mask != 0
    });
    items.sort_unstable_by_key(|item| item.form_id);
    let mut normalized: Vec<AppearanceItem> = Vec::with_capacity(items.len());
    for item in items.drain(..) {
        if let Some(previous) = normalized
            .last_mut()
            .filter(|previous| previous.form_id == item.form_id)
        {
            previous.slot_mask |= item.slot_mask;
        } else {
            normalized.push(item);
        }
    }
    *items = normalized;
}

/// Weighted Jaccard similarity of two outfits, in 0..=1.
///
/// Categories absent from both outfits are excluded and their weight is not
/// counted, so comparing two unarmoured Players is not penalised for the helmet
/// that neither of them wears.
pub fn outfit_similarity(
    left: &[AppearanceItem],
    right: &[AppearanceItem],
    weights: &AppearanceWeights,
) -> f32 {
    let mut score = 0.0_f32;
    let mut applicable_weight = 0.0_f32;
    for (category, category_mask) in APPEARANCE_CATEGORIES {
        let left_ids: Vec<_> = left
            .iter()
            .filter(|item| item.slot_mask & category_mask != 0)
            .map(|item| item.form_id)
            .collect();
        let right_ids: Vec<_> = right
            .iter()
            .filter(|item| item.slot_mask & category_mask != 0)
            .map(|item| item.form_id)
            .collect();
        if left_ids.is_empty() && right_ids.is_empty() {
            continue;
        }
        let weight = weights.get(category);
        applicable_weight += weight;
        let intersection = left_ids.iter().filter(|id| right_ids.contains(id)).count() as f32;
        let union = (left_ids.len() + right_ids.len()) as f32 - intersection;
        if union > 0.0 {
            score += weight * (intersection / union);
        }
    }
    if applicable_weight > 0.0 {
        score / applicable_weight
    } else if left == right {
        1.0
    } else {
        0.0
    }
}

#[cfg(test)]
mod tests;
