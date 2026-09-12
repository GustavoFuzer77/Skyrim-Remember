//! Impressions: what the Player's equipment makes an NPC feel, declared as data.
//!
//! Three decisions shape this module:
//!
//! - **Rules match keywords first.** `ArmorMaterialHide` covers every hide armor
//!   of every mod. A specific Form is the fallback for items that carry no
//!   distinguishing keyword, like vanilla bandit armor.
//! - **Equipment is a state, not an event.** Wearing something implies a level
//!   an axis is raised to, never a delta added on every scan: the host polls,
//!   and an additive rule would saturate every axis within seconds.
//! - **Rules project, temperaments convert.** A rule says what an item suggests
//!   (`banditry`, `threat`); each profile's temperament turns that into its own
//!   axes. A new rule reaches every profile, and a new profile reuses every
//!   rule.
//!
//! Nothing here knows about Skyrim. The host resolves every [`RuleReference`]
//! to a runtime FormID and passes worn Forms and keywords as plain integers.

use crate::appearance::AppearanceItem;
use crate::memory::{Axis, MemoryStore, NpcId, AXIS_CAPACITY};
use crate::tuning::MAX_MEMORY_VALUE;
use serde::Deserialize;
use std::collections::{BTreeMap, HashMap};
use std::fmt;

pub const PROFILE_COUNT: usize = 5;

/// Hard ABI cap on the keywords of one observation, like
/// [`crate::MAX_APPEARANCE_ITEMS`] is for worn items.
pub const MAX_WORN_KEYWORDS: usize = 512;

/// Largest rule file the host may hand over. Rule files are small; anything
/// near this is a mistake, not a configuration.
pub const MAX_RULE_FILE_BYTES: usize = 1024 * 1024;

/// The smallest rise worth reporting. A worn item keeps topping its axis back up
/// as memory decays between scans, and those tiny corrections are not news.
pub const NOTICEABLE_RISE: f32 = 1.0;

/// Mirrors the host's `NpcProfile`. The numbers are ABI.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Profile {
    Guard = 0,
    Follower = 1,
    Citizen = 2,
    Merchant = 3,
    Generic = 4,
}

impl Profile {
    pub const ALL: [Profile; PROFILE_COUNT] = [
        Profile::Guard,
        Profile::Follower,
        Profile::Citizen,
        Profile::Merchant,
        Profile::Generic,
    ];

    pub fn from_u32(value: u32) -> Option<Self> {
        usize::try_from(value)
            .ok()
            .and_then(|index| Self::ALL.get(index).copied())
    }

    /// The name rule files use.
    pub const fn name(self) -> &'static str {
        match self {
            Profile::Guard => "guard",
            Profile::Follower => "follower",
            Profile::Citizen => "citizen",
            Profile::Merchant => "merchant",
            Profile::Generic => "generic",
        }
    }

    fn from_name(name: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|profile| profile.name() == name)
    }
}

/// Something a rule matches that only the host can turn into a runtime FormID.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum RuleReference {
    /// A keyword, by EditorID.
    Keyword(String),
    /// One Form, by the plugin that defines it and its ID local to that plugin.
    Form { plugin: String, local_id: u32 },
}

/// Why a rule file was rejected, phrased for the person who wrote it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RulesError(String);

impl fmt::Display for RulesError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

/// The axis levels one outfit implies for one profile.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct ImpressionOutcome {
    pub targets: [f32; AXIS_CAPACITY],
    pub matched_rules: u32,
}

/// What applying an impression changed in one NPC's memory.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct ImpressionObservation {
    pub targets: [f32; AXIS_CAPACITY],
    /// One bit per axis slot that rose by at least [`NOTICEABLE_RISE`].
    pub raised_axes: u32,
    pub matched_rules: u32,
}

#[derive(Debug)]
struct Impression {
    /// Projection index and strength.
    projections: Vec<(usize, f32)>,
}

/// Every loaded rule, and which runtime FormIDs trigger them.
#[derive(Debug, Default)]
pub struct Rules {
    impressions: Vec<Impression>,
    references: Vec<RuleReference>,
    reference_index: HashMap<RuleReference, usize>,
    /// Impressions each reference triggers.
    triggers: Vec<Vec<usize>>,
    /// Runtime FormID of each reference, once the host resolved it.
    bindings: Vec<Option<u32>>,
    /// Runtime FormID to the impressions it triggers. Keywords and Forms share
    /// one FormID space, so one map serves both.
    bound: HashMap<u32, Vec<usize>>,
    projections: Vec<String>,
    projection_index: HashMap<String, usize>,
    /// Per profile: projection index to the axis weights it converts into.
    temperaments: [HashMap<usize, Vec<(Axis, f32)>>; PROFILE_COUNT],
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RuleFile {
    #[serde(default)]
    impressions: Vec<ImpressionEntry>,
    #[serde(default)]
    temperaments: BTreeMap<String, BTreeMap<String, BTreeMap<String, f32>>>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ImpressionEntry {
    #[serde(default)]
    name: Option<String>,
    #[serde(default)]
    keywords: Vec<String>,
    #[serde(default)]
    forms: Vec<String>,
    projects: BTreeMap<String, f32>,
}

struct StagedImpression {
    references: Vec<RuleReference>,
    projections: Vec<(String, f32)>,
}

struct StagedTemperament {
    profile: Profile,
    projection: String,
    weights: Vec<(Axis, f32)>,
}

fn error(message: impl Into<String>) -> RulesError {
    RulesError(message.into())
}

/// Parses `0x06F393~Skyrim.esm`, the notation SPID and KID users already know.
fn parse_form(text: &str) -> Result<RuleReference, String> {
    let (id, plugin) = text
        .split_once('~')
        .ok_or_else(|| format!("form \"{text}\" must look like 0x06F393~Skyrim.esm"))?;
    let (id, plugin) = (id.trim(), plugin.trim());
    let digits = id
        .strip_prefix("0x")
        .or_else(|| id.strip_prefix("0X"))
        .unwrap_or(id);
    let local_id = u32::from_str_radix(digits, 16)
        .map_err(|_| format!("form \"{text}\" has an invalid hexadecimal id"))?;
    if local_id == 0 || local_id > 0x00FF_FFFF {
        return Err(format!(
            "form \"{text}\" id must be a local id between 0x1 and 0xFFFFFF"
        ));
    }
    if plugin.is_empty() {
        return Err(format!("form \"{text}\" names no plugin"));
    }
    Ok(RuleReference::Form {
        plugin: plugin.to_owned(),
        local_id,
    })
}

fn valid_strength(value: f32) -> bool {
    value.is_finite() && (0.0..=MAX_MEMORY_VALUE).contains(&value)
}

impl Rules {
    /// Adds one rule file. The whole file is validated first, so a rejected
    /// file changes nothing. Impressions accumulate across files; a later file's
    /// temperament entry replaces an earlier one for the same profile and
    /// projection, which is how a mod retunes the defaults.
    pub fn load_json(&mut self, bytes: &[u8]) -> Result<(), RulesError> {
        let file: RuleFile =
            serde_json::from_slice(bytes).map_err(|parse| error(parse.to_string()))?;

        let mut impressions = Vec::with_capacity(file.impressions.len());
        for (position, entry) in file.impressions.into_iter().enumerate() {
            let label = entry
                .name
                .clone()
                .unwrap_or_else(|| format!("impressions[{position}]"));
            if entry.keywords.is_empty() && entry.forms.is_empty() {
                return Err(error(format!(
                    "{label}: needs at least one keyword or form"
                )));
            }
            if entry.projects.is_empty() {
                return Err(error(format!(
                    "{label}: \"projects\" needs at least one projection"
                )));
            }
            let mut references = Vec::with_capacity(entry.keywords.len() + entry.forms.len());
            for keyword in entry.keywords {
                if keyword.trim().is_empty() {
                    return Err(error(format!("{label}: empty keyword")));
                }
                references.push(RuleReference::Keyword(keyword.trim().to_owned()));
            }
            for form in &entry.forms {
                references.push(
                    parse_form(form).map_err(|message| error(format!("{label}: {message}")))?,
                );
            }
            let mut projections = Vec::with_capacity(entry.projects.len());
            for (projection, strength) in entry.projects {
                if projection.trim().is_empty() {
                    return Err(error(format!("{label}: empty projection name")));
                }
                if !valid_strength(strength) {
                    return Err(error(format!(
                        "{label}: projection \"{projection}\" strength {strength} must be between 0 and {MAX_MEMORY_VALUE}"
                    )));
                }
                projections.push((projection, strength));
            }
            impressions.push(StagedImpression {
                references,
                projections,
            });
        }

        let mut temperaments = Vec::new();
        for (profile_name, projections) in file.temperaments {
            let profile = Profile::from_name(&profile_name).ok_or_else(|| {
                error(format!(
                    "temperaments: unknown profile \"{profile_name}\", expected guard, follower, citizen, merchant or generic"
                ))
            })?;
            for (projection, axes) in projections {
                let mut weights = Vec::with_capacity(axes.len());
                for (axis_name, weight) in axes {
                    let axis = Axis::ALL
                        .into_iter()
                        .find(|axis| axis.name() == axis_name)
                        .ok_or_else(|| {
                            error(format!(
                                "temperaments.{profile_name}.{projection}: unknown axis \"{axis_name}\""
                            ))
                        })?;
                    if !weight.is_finite() || weight < 0.0 {
                        return Err(error(format!(
                            "temperaments.{profile_name}.{projection}.{axis_name}: weight {weight} must be zero or positive"
                        )));
                    }
                    weights.push((axis, weight));
                }
                temperaments.push(StagedTemperament {
                    profile,
                    projection,
                    weights,
                });
            }
        }

        for staged in impressions {
            let impression = self.impressions.len();
            let projections = staged
                .projections
                .into_iter()
                .map(|(name, strength)| (self.intern_projection(name), strength))
                .collect();
            self.impressions.push(Impression { projections });
            for reference in staged.references {
                let index = self.intern_reference(reference);
                if !self.triggers[index].contains(&impression) {
                    self.triggers[index].push(impression);
                }
            }
        }
        for staged in temperaments {
            let projection = self.intern_projection(staged.projection);
            self.temperaments[staged.profile as usize].insert(projection, staged.weights);
        }
        self.rebuild_bound();
        Ok(())
    }

    pub fn impression_count(&self) -> usize {
        self.impressions.len()
    }

    /// Every distinct keyword and Form the loaded rules mention, in load order.
    pub fn references(&self) -> &[RuleReference] {
        &self.references
    }

    pub fn bound_count(&self) -> usize {
        self.bindings
            .iter()
            .filter(|binding| binding.is_some())
            .count()
    }

    /// Records the runtime FormID the host resolved a reference to. Zero marks it
    /// unresolved. Returns false for an index no rule produced.
    pub fn bind(&mut self, reference: usize, runtime_form_id: u32) -> bool {
        let Some(binding) = self.bindings.get_mut(reference) else {
            return false;
        };
        *binding = (runtime_form_id != 0).then_some(runtime_form_id);
        self.rebuild_bound();
        true
    }

    /// The axis levels this outfit implies for `profile`.
    ///
    /// Within one projection the strongest matching rule wins, and across
    /// projections each axis takes the highest converted level. Nothing adds up,
    /// so wearing three bandit pieces is no more suspicious than the most
    /// suspicious one, and the result is the same however often it is asked.
    pub fn evaluate(
        &self,
        profile: Profile,
        items: &[AppearanceItem],
        keywords: &[u32],
    ) -> ImpressionOutcome {
        let mut outcome = ImpressionOutcome::default();
        if self.bound.is_empty() {
            return outcome;
        }
        let mut matched = vec![false; self.impressions.len()];
        let worn = items
            .iter()
            .map(|item| item.form_id)
            .chain(keywords.iter().copied());
        for form_id in worn {
            if let Some(impressions) = self.bound.get(&form_id) {
                for &impression in impressions {
                    matched[impression] = true;
                }
            }
        }

        let mut strengths = vec![0.0_f32; self.projections.len()];
        for (impression, _) in matched.iter().enumerate().filter(|(_, hit)| **hit) {
            outcome.matched_rules += 1;
            for &(projection, strength) in &self.impressions[impression].projections {
                strengths[projection] = strengths[projection].max(strength);
            }
        }

        let temperament = &self.temperaments[profile as usize];
        for (projection, strength) in strengths.into_iter().enumerate() {
            if strength <= 0.0 {
                continue;
            }
            let Some(weights) = temperament.get(&projection) else {
                continue;
            };
            for &(axis, weight) in weights {
                let level = (strength * weight).clamp(0.0, MAX_MEMORY_VALUE);
                let target = &mut outcome.targets[axis.index()];
                *target = target.max(level);
            }
        }
        outcome
    }

    fn intern_projection(&mut self, name: String) -> usize {
        if let Some(&index) = self.projection_index.get(&name) {
            return index;
        }
        let index = self.projections.len();
        self.projections.push(name.clone());
        self.projection_index.insert(name, index);
        index
    }

    fn intern_reference(&mut self, reference: RuleReference) -> usize {
        if let Some(&index) = self.reference_index.get(&reference) {
            return index;
        }
        let index = self.references.len();
        self.references.push(reference.clone());
        self.reference_index.insert(reference, index);
        self.triggers.push(Vec::new());
        self.bindings.push(None);
        index
    }

    fn rebuild_bound(&mut self) {
        self.bound.clear();
        for (reference, binding) in self.bindings.iter().enumerate() {
            let Some(form_id) = binding else {
                continue;
            };
            let impressions = self.bound.entry(*form_id).or_default();
            for &impression in &self.triggers[reference] {
                if !impressions.contains(&impression) {
                    impressions.push(impression);
                }
            }
        }
        self.bound.retain(|_, impressions| !impressions.is_empty());
    }
}

impl MemoryStore {
    pub fn rules(&self) -> &Rules {
        &self.rules
    }

    pub fn rules_mut(&mut self) -> &mut Rules {
        &mut self.rules
    }

    /// Raises one NPC's axes to the levels the Player's outfit implies for its
    /// profile. Never lowers anything, so a stronger feeling from an earlier
    /// event survives an outfit that implies less.
    ///
    /// The caller decides attribution: call this only when the NPC recognized
    /// the Player, or a masked stranger's outfit would be held against them.
    pub fn observe_impression(
        &mut self,
        npc_id: NpcId,
        profile: Profile,
        items: &[AppearanceItem],
        keywords: &[u32],
        game_days: f64,
    ) -> Option<ImpressionObservation> {
        let outcome = self.rules.evaluate(profile, items, keywords);
        let memory = self.touch(npc_id, game_days)?;
        let mut raised_axes = 0_u32;
        for (slot, &target) in outcome.targets.iter().enumerate() {
            let value = &mut memory.axes[slot];
            if target > *value {
                if target - *value >= NOTICEABLE_RISE {
                    raised_axes |= 1 << slot;
                }
                *value = target;
            }
        }
        Some(ImpressionObservation {
            targets: outcome.targets,
            raised_axes,
            matched_rules: outcome.matched_rules,
        })
    }
}

#[cfg(test)]
mod tests;
