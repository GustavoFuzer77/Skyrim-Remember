use crate::memory::{Axis, AXIS_CAPACITY};
use crate::persistence::{self, PersistenceError};
use crate::rules::{Profile, RuleReference, MAX_RULE_FILE_BYTES, MAX_WORN_KEYWORDS};
use crate::tuning::{Tuning, MAX_APPEARANCE_ITEMS};
use crate::{AppearanceItem, MemoryStore, NpcId, NpcMemory};
use std::ffi::c_void;
use std::ops::{Index, IndexMut};
use std::slice;
use std::sync::Mutex;

/// Opaque to C++. Allocate and release only through this module's C API.
///
/// The store owns the [`Tuning`] every algorithm reads, so this type is the
/// single place a future configuration setter has to write to.
pub struct MemoryEngine {
    store: Mutex<MemoryStore>,
}

#[repr(u32)]
#[derive(Debug, PartialEq, Eq)]
pub enum BridgeStatus {
    Ok = 0,
    Created = 1,
    InvalidId = 2,
    NotFound = 3,
    InvalidArgument = 4,
    Unavailable = 5,
    BufferTooSmall = 6,
    InvalidData = 7,
    UnsupportedVersion = 8,
}

/// Copied into caller-owned storage; no Rust allocation crosses the ABI.
///
/// Indexed by `Axis`, which both sides mirror. Slots beyond the named axes are
/// reserve and read as zero, so naming a new axis does not resize this struct.
#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct MemorySnapshot {
    pub axes: [f32; AXIS_CAPACITY],
    pub last_decay_game_days: f64,
}

impl From<NpcMemory> for MemorySnapshot {
    fn from(memory: NpcMemory) -> Self {
        Self {
            axes: memory.axes,
            last_decay_game_days: memory.last_decay_game_days,
        }
    }
}

impl Index<Axis> for MemorySnapshot {
    type Output = f32;

    fn index(&self, axis: Axis) -> &f32 {
        &self.axes[axis.index()]
    }
}

/// One change per axis slot. Leaving a slot at zero leaves that axis alone.
#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct MemoryDelta {
    pub axes: [f32; AXIS_CAPACITY],
}

impl MemoryDelta {
    /// A delta that touches exactly one axis.
    pub fn of(axis: Axis, value: f32) -> Self {
        let mut delta = Self::default();
        delta[axis] = value;
        delta
    }
}

impl Index<Axis> for MemoryDelta {
    type Output = f32;

    fn index(&self, axis: Axis) -> &f32 {
        &self.axes[axis.index()]
    }
}

impl IndexMut<Axis> for MemoryDelta {
    fn index_mut(&mut self, axis: Axis) -> &mut f32 {
        &mut self.axes[axis.index()]
    }
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct PersistenceStats {
    pub loaded: u32,
    pub unresolved: u32,
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct AppearanceResult {
    pub outfit_similarity: f32,
    pub identity_confidence: f32,
    pub recognition_threshold: f32,
    pub appearance_matched: u32,
    pub recognized: u32,
    pub linked_by_continuity: u32,
    pub learned: u32,
    pub face_visible: u32,
    pub item_count: u32,
    pub known_appearances: u32,
}

/// What one outfit did to one NPC's axes.
#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct ImpressionResult {
    /// The level each axis slot was raised to, or zero when nothing applied.
    pub targets: [f32; AXIS_CAPACITY],
    /// One bit per axis slot that rose noticeably.
    pub raised_axes: u32,
    pub matched_rules: u32,
}

/// `kind` of a [`RuleReferenceInfo`].
pub const RULE_REFERENCE_KEYWORD: u32 = 0;
pub const RULE_REFERENCE_FORM: u32 = 1;

/// Describes one reference the host must resolve. The name — a keyword
/// EditorID or a plugin file name — is copied into a separate caller buffer.
#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct RuleReferenceInfo {
    pub kind: u32,
    /// Local FormID inside the plugin for a Form; zero for a keyword.
    pub local_form_id: u32,
    pub name_length: u32,
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct RulesSummary {
    pub impressions: u32,
    pub references: u32,
    pub bound_references: u32,
}

pub type FormIdResolver =
    unsafe extern "C" fn(context: *mut c_void, old_form_id: u32, new_form_id: *mut u32) -> bool;

fn valid_game_days(game_days: f64) -> bool {
    game_days.is_finite() && game_days >= 0.0
}

fn valid_delta(delta: MemoryDelta) -> bool {
    delta.axes.iter().all(|value| value.is_finite())
}

/// Validation shared by both appearance entry points.
///
/// Keeping it in one place is what guarantees that `observe` and `evaluate`
/// cannot drift apart the next time an argument rule changes.
///
/// # Safety
/// `items` must point to `item_count` readable `AppearanceItem`s when
/// `item_count` is non-zero.
unsafe fn appearance_arguments(
    reference_form_id: u32,
    items: *const AppearanceItem,
    item_count: usize,
    face_visible: u32,
    visual_continuity: u32,
    output: *const AppearanceResult,
) -> Result<(NpcId, Vec<AppearanceItem>), BridgeStatus> {
    if output.is_null()
        || item_count > MAX_APPEARANCE_ITEMS
        || face_visible > 1
        || visual_continuity > 1
        || (item_count > 0 && items.is_null())
    {
        return Err(BridgeStatus::InvalidArgument);
    }
    let Some(id) = NpcId::new(reference_form_id) else {
        return Err(BridgeStatus::InvalidId);
    };
    let appearance_items = if item_count == 0 {
        Vec::new()
    } else {
        // SAFETY: caller guarantees `item_count` readable AppearanceItems.
        unsafe { slice::from_raw_parts(items, item_count) }.to_vec()
    };
    Ok((id, appearance_items))
}

fn appearance_result(
    observation: crate::appearance::AppearanceObservation,
    face_visible: u32,
) -> AppearanceResult {
    AppearanceResult {
        outfit_similarity: observation.outfit_similarity,
        identity_confidence: observation.identity_confidence,
        recognition_threshold: observation.recognition_threshold,
        appearance_matched: u32::from(observation.appearance_matched),
        recognized: u32::from(observation.recognized),
        linked_by_continuity: u32::from(observation.linked_by_continuity),
        learned: u32::from(observation.learned),
        face_visible,
        item_count: observation.item_count,
        known_appearances: observation.known_appearances,
    }
}

#[no_mangle]
pub extern "C" fn skyrim_remember_create() -> *mut MemoryEngine {
    Box::into_raw(Box::new(MemoryEngine {
        store: Mutex::new(MemoryStore::new()),
    }))
}

/// Releases the context. Null is accepted as a no-op.
///
/// # Safety
/// A non-null pointer must come from `skyrim_remember_create`, must not have
/// been freed, and must not be used concurrently or after this call.
#[no_mangle]
pub unsafe extern "C" fn skyrim_remember_destroy(engine: *mut MemoryEngine) {
    if !engine.is_null() {
        // SAFETY: ownership is transferred back by the caller exactly once.
        drop(unsafe { Box::from_raw(engine) });
    }
}

/// Registers a reference without changing any existing memory values.
///
/// # Safety
/// A non-null engine must be a live context returned by `skyrim_remember_create`
/// and must remain alive for this call. Concurrent store calls are supported.
#[no_mangle]
pub unsafe extern "C" fn skyrim_remember_register(
    engine: *const MemoryEngine,
    reference_form_id: u32,
    game_days: f64,
) -> BridgeStatus {
    // SAFETY: the caller guarantees validity of a non-null engine.
    let Some(engine) = (unsafe { engine.as_ref() }) else {
        return BridgeStatus::InvalidArgument;
    };
    let Some(id) = NpcId::new(reference_form_id) else {
        return BridgeStatus::InvalidId;
    };
    if !valid_game_days(game_days) {
        return BridgeStatus::InvalidArgument;
    }
    let Ok(mut store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    if store.register(id, game_days) {
        BridgeStatus::Created
    } else {
        BridgeStatus::Ok
    }
}

/// Reads a snapshot; errors leave the output untouched and never create entries.
///
/// # Safety
/// A non-null engine must be live for this call. A non-null output must point
/// to aligned, writable storage for one MemorySnapshot, not overlap the engine,
/// and not be accessed by another thread during this call.
#[no_mangle]
pub unsafe extern "C" fn skyrim_remember_get(
    engine: *const MemoryEngine,
    reference_form_id: u32,
    game_days: f64,
    output: *mut MemorySnapshot,
) -> BridgeStatus {
    if output.is_null() {
        return BridgeStatus::InvalidArgument;
    }
    // SAFETY: the caller guarantees validity of a non-null engine.
    let Some(engine) = (unsafe { engine.as_ref() }) else {
        return BridgeStatus::InvalidArgument;
    };
    let Some(id) = NpcId::new(reference_form_id) else {
        return BridgeStatus::InvalidId;
    };
    if !valid_game_days(game_days) {
        return BridgeStatus::InvalidArgument;
    }
    let Ok(mut store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    let Some(memory) = store.touch(id, game_days) else {
        return BridgeStatus::NotFound;
    };
    // SAFETY: caller owns writable output storage; the snapshot has no pointers.
    unsafe { output.write((*memory).into()) };
    BridgeStatus::Ok
}

/// Applies a gameplay delta after bringing the memory forward to `game_days`.
/// Values are clamped to the inclusive 0..=100 range.
///
/// # Safety
/// All non-null pointers must remain valid for the duration of the call. The
/// output must be exclusively writable and must not overlap the engine.
#[no_mangle]
pub unsafe extern "C" fn skyrim_remember_apply_delta(
    engine: *const MemoryEngine,
    reference_form_id: u32,
    game_days: f64,
    delta: MemoryDelta,
    output: *mut MemorySnapshot,
) -> BridgeStatus {
    if output.is_null() || !valid_game_days(game_days) || !valid_delta(delta) {
        return BridgeStatus::InvalidArgument;
    }
    // SAFETY: the caller guarantees validity of a non-null engine.
    let Some(engine) = (unsafe { engine.as_ref() }) else {
        return BridgeStatus::InvalidArgument;
    };
    let Some(id) = NpcId::new(reference_form_id) else {
        return BridgeStatus::InvalidId;
    };
    let Ok(mut store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    let memory = store.touch_or_create(id, game_days);
    memory.add(&delta.axes);
    // SAFETY: caller owns writable output storage; the snapshot has no pointers.
    unsafe { output.write((*memory).into()) };
    BridgeStatus::Ok
}

/// Compares the player's current equipment with the appearances known by one
/// NPC. A first observation is learned; later novel appearances are learned
/// only when the NPC can connect them to a known face, outfit, or uninterrupted
/// visual contact supplied by the host.
///
/// # Safety
/// `items` must point to `item_count` readable AppearanceItems when non-empty.
/// All remaining pointer requirements match the other engine calls.
#[no_mangle]
pub unsafe extern "C" fn skyrim_remember_observe_appearance(
    engine: *const MemoryEngine,
    reference_form_id: u32,
    game_days: f64,
    items: *const AppearanceItem,
    item_count: usize,
    face_visible: u32,
    visual_continuity: u32,
    output: *mut AppearanceResult,
) -> BridgeStatus {
    if !valid_game_days(game_days) {
        return BridgeStatus::InvalidArgument;
    }
    // SAFETY: the caller guarantees validity of a non-null engine.
    let Some(engine) = (unsafe { engine.as_ref() }) else {
        return BridgeStatus::InvalidArgument;
    };
    // SAFETY: the item buffer contract is forwarded unchanged to the helper.
    let (id, appearance_items) = match unsafe {
        appearance_arguments(
            reference_form_id,
            items,
            item_count,
            face_visible,
            visual_continuity,
            output,
        )
    } {
        Ok(arguments) => arguments,
        Err(status) => return status,
    };
    let Ok(mut store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    if store.get(id).is_none() {
        return BridgeStatus::NotFound;
    }
    let Some(observation) = store.observe_appearance(
        id,
        appearance_items,
        face_visible != 0,
        visual_continuity != 0,
        game_days,
    ) else {
        return BridgeStatus::InvalidArgument;
    };
    // SAFETY: output points to one exclusively writable AppearanceResult.
    unsafe { output.write(appearance_result(observation, face_visible)) };
    BridgeStatus::Ok
}

/// Evaluates recognition without learning, refreshing, or creating anything.
///
/// # Safety
/// Pointer requirements are identical to `skyrim_remember_observe_appearance`.
#[no_mangle]
pub unsafe extern "C" fn skyrim_remember_evaluate_appearance(
    engine: *const MemoryEngine,
    reference_form_id: u32,
    items: *const AppearanceItem,
    item_count: usize,
    face_visible: u32,
    visual_continuity: u32,
    output: *mut AppearanceResult,
) -> BridgeStatus {
    // SAFETY: the caller guarantees validity of a non-null engine.
    let Some(engine) = (unsafe { engine.as_ref() }) else {
        return BridgeStatus::InvalidArgument;
    };
    // SAFETY: the item buffer contract is forwarded unchanged to the helper.
    let (id, appearance_items) = match unsafe {
        appearance_arguments(
            reference_form_id,
            items,
            item_count,
            face_visible,
            visual_continuity,
            output,
        )
    } {
        Ok(arguments) => arguments,
        Err(status) => return status,
    };
    let Ok(store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    let Some(observation) = store.evaluate_appearance(
        id,
        appearance_items,
        face_visible != 0,
        visual_continuity != 0,
    ) else {
        return if store.get(id).is_some() {
            BridgeStatus::InvalidArgument
        } else {
            BridgeStatus::NotFound
        };
    };
    // SAFETY: output points to one exclusively writable AppearanceResult.
    unsafe { output.write(appearance_result(observation, face_visible)) };
    BridgeStatus::Ok
}

fn count(value: usize) -> u32 {
    u32::try_from(value).unwrap_or(u32::MAX)
}

/// Adds one JSON rule file. A rejected file changes nothing and, when an error
/// buffer is supplied, explains why in a NUL-terminated, possibly truncated
/// message.
///
/// # Safety
/// `data` must point to `length` readable bytes. A non-null `error` must point
/// to `error_capacity` writable bytes. The engine requirements match the other
/// engine calls.
#[no_mangle]
pub unsafe extern "C" fn skyrim_remember_load_rules(
    engine: *const MemoryEngine,
    data: *const u8,
    length: usize,
    error: *mut u8,
    error_capacity: usize,
) -> BridgeStatus {
    if data.is_null() || length == 0 || length > MAX_RULE_FILE_BYTES {
        return BridgeStatus::InvalidArgument;
    }
    // SAFETY: the caller guarantees validity of a non-null engine.
    let Some(engine) = (unsafe { engine.as_ref() }) else {
        return BridgeStatus::InvalidArgument;
    };
    // SAFETY: the caller guarantees `length` readable bytes.
    let bytes = unsafe { slice::from_raw_parts(data, length) };
    let Ok(mut store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    match store.rules_mut().load_json(bytes) {
        Ok(()) => BridgeStatus::Ok,
        Err(rejection) => {
            if !error.is_null() && error_capacity > 0 {
                let message = rejection.to_string();
                let copied = message.len().min(error_capacity - 1);
                // SAFETY: `copied + 1 <= error_capacity` bytes are writable, and a
                // Rust String never overlaps caller storage.
                unsafe {
                    std::ptr::copy_nonoverlapping(message.as_ptr(), error, copied);
                    error.add(copied).write(0);
                }
            }
            BridgeStatus::InvalidData
        }
    }
}

/// # Safety
/// A non-null output must point to one exclusively writable RulesSummary.
#[no_mangle]
pub unsafe extern "C" fn skyrim_remember_rules_summary(
    engine: *const MemoryEngine,
    output: *mut RulesSummary,
) -> BridgeStatus {
    if output.is_null() {
        return BridgeStatus::InvalidArgument;
    }
    // SAFETY: the caller guarantees validity of a non-null engine.
    let Some(engine) = (unsafe { engine.as_ref() }) else {
        return BridgeStatus::InvalidArgument;
    };
    let Ok(store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    let rules = store.rules();
    let summary = RulesSummary {
        impressions: count(rules.impression_count()),
        references: count(rules.references().len()),
        bound_references: count(rules.bound_count()),
    };
    // SAFETY: output points to one exclusively writable RulesSummary.
    unsafe { output.write(summary) };
    BridgeStatus::Ok
}

/// Describes reference `index`. `output` is written even when the name buffer
/// is too small, so the caller can size it and ask again.
///
/// # Safety
/// `output` must point to one writable RuleReferenceInfo. A non-null `name`
/// must point to `name_capacity` writable bytes. The name is not NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn skyrim_remember_rule_reference(
    engine: *const MemoryEngine,
    index: u32,
    output: *mut RuleReferenceInfo,
    name: *mut u8,
    name_capacity: usize,
) -> BridgeStatus {
    if output.is_null() {
        return BridgeStatus::InvalidArgument;
    }
    // SAFETY: the caller guarantees validity of a non-null engine.
    let Some(engine) = (unsafe { engine.as_ref() }) else {
        return BridgeStatus::InvalidArgument;
    };
    let Ok(store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    let Some(reference) = usize::try_from(index)
        .ok()
        .and_then(|index| store.rules().references().get(index))
    else {
        return BridgeStatus::NotFound;
    };
    let (kind, local_form_id, text) = match reference {
        RuleReference::Keyword(editor_id) => (RULE_REFERENCE_KEYWORD, 0, editor_id.as_str()),
        RuleReference::Form { plugin, local_id } => {
            (RULE_REFERENCE_FORM, *local_id, plugin.as_str())
        }
    };
    let info = RuleReferenceInfo {
        kind,
        local_form_id,
        name_length: count(text.len()),
    };
    // SAFETY: output points to one exclusively writable RuleReferenceInfo.
    unsafe { output.write(info) };
    if name.is_null() || name_capacity < text.len() {
        return BridgeStatus::BufferTooSmall;
    }
    // SAFETY: `name` has room for `text.len()` bytes and does not overlap Rust
    // storage.
    unsafe { std::ptr::copy_nonoverlapping(text.as_ptr(), name, text.len()) };
    BridgeStatus::Ok
}

/// Records what reference `index` resolved to in the running game. Zero marks
/// it unresolved.
///
/// # Safety
/// The engine requirements match the other engine calls.
#[no_mangle]
pub unsafe extern "C" fn skyrim_remember_bind_rule_reference(
    engine: *const MemoryEngine,
    index: u32,
    runtime_form_id: u32,
) -> BridgeStatus {
    // SAFETY: the caller guarantees validity of a non-null engine.
    let Some(engine) = (unsafe { engine.as_ref() }) else {
        return BridgeStatus::InvalidArgument;
    };
    let Ok(mut store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    let bound = usize::try_from(index)
        .map(|index| store.rules_mut().bind(index, runtime_form_id))
        .unwrap_or(false);
    if bound {
        BridgeStatus::Ok
    } else {
        BridgeStatus::NotFound
    }
}

/// Raises one NPC's axes to what the Player's worn items and keywords imply for
/// `profile`. Call only for an NPC that recognized the Player.
///
/// # Safety
/// `items` must point to `item_count` readable AppearanceItems and `keywords`
/// to `keyword_count` readable FormIDs when non-empty. `output` must point to
/// one exclusively writable ImpressionResult.
#[no_mangle]
pub unsafe extern "C" fn skyrim_remember_observe_impression(
    engine: *const MemoryEngine,
    reference_form_id: u32,
    game_days: f64,
    profile: u32,
    items: *const AppearanceItem,
    item_count: usize,
    keywords: *const u32,
    keyword_count: usize,
    output: *mut ImpressionResult,
) -> BridgeStatus {
    if output.is_null()
        || !valid_game_days(game_days)
        || item_count > MAX_APPEARANCE_ITEMS
        || keyword_count > MAX_WORN_KEYWORDS
        || (item_count > 0 && items.is_null())
        || (keyword_count > 0 && keywords.is_null())
    {
        return BridgeStatus::InvalidArgument;
    }
    let Some(profile) = Profile::from_u32(profile) else {
        return BridgeStatus::InvalidArgument;
    };
    // SAFETY: the caller guarantees validity of a non-null engine.
    let Some(engine) = (unsafe { engine.as_ref() }) else {
        return BridgeStatus::InvalidArgument;
    };
    let Some(id) = NpcId::new(reference_form_id) else {
        return BridgeStatus::InvalidId;
    };
    let items = if item_count == 0 {
        &[][..]
    } else {
        // SAFETY: caller guarantees `item_count` readable AppearanceItems.
        unsafe { slice::from_raw_parts(items, item_count) }
    };
    let keywords = if keyword_count == 0 {
        &[][..]
    } else {
        // SAFETY: caller guarantees `keyword_count` readable FormIDs.
        unsafe { slice::from_raw_parts(keywords, keyword_count) }
    };
    let Ok(mut store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    let Some(observation) = store.observe_impression(id, profile, items, keywords, game_days)
    else {
        return BridgeStatus::NotFound;
    };
    let result = ImpressionResult {
        targets: observation.targets,
        raised_axes: observation.raised_axes,
        matched_rules: observation.matched_rules,
    };
    // SAFETY: output points to one exclusively writable ImpressionResult.
    unsafe { output.write(result) };
    BridgeStatus::Ok
}

/// Serializes the complete store. Call once with a null output to obtain the
/// required byte count, then again with a buffer of at least that size.
///
/// # Safety
/// Pointers must be valid for their documented sizes and lifetimes. `written`
/// is required and must be exclusively writable.
#[no_mangle]
pub unsafe extern "C" fn skyrim_remember_serialize(
    engine: *const MemoryEngine,
    output: *mut u8,
    capacity: usize,
    written: *mut usize,
) -> BridgeStatus {
    if written.is_null() || (output.is_null() && capacity != 0) {
        return BridgeStatus::InvalidArgument;
    }
    // SAFETY: the caller guarantees validity of a non-null engine.
    let Some(engine) = (unsafe { engine.as_ref() }) else {
        return BridgeStatus::InvalidArgument;
    };
    let Ok(store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    let Ok(bytes) = persistence::serialize(&store) else {
        return BridgeStatus::InvalidData;
    };
    // SAFETY: written is required to point to one exclusively writable usize.
    unsafe { written.write(bytes.len()) };
    if output.is_null() {
        return BridgeStatus::Ok;
    }
    if capacity < bytes.len() {
        return BridgeStatus::BufferTooSmall;
    }
    // SAFETY: the caller promises output is writable for `capacity` bytes.
    unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), output, bytes.len()) };
    BridgeStatus::Ok
}

/// Atomically replaces the store from serialized data after resolving every
/// saved FormID through the host-provided SKSE resolver. Unresolved entries are
/// skipped and counted; malformed input leaves the previous store untouched.
///
/// # Safety
/// The byte buffer, engine, resolver, resolver context, and stats output must
/// satisfy their respective host contracts for the duration of this call.
#[no_mangle]
pub unsafe extern "C" fn skyrim_remember_deserialize(
    engine: *const MemoryEngine,
    data: *const u8,
    length: usize,
    resolver: Option<FormIdResolver>,
    resolver_context: *mut c_void,
    output: *mut PersistenceStats,
) -> BridgeStatus {
    if data.is_null() || output.is_null() {
        return BridgeStatus::InvalidArgument;
    }
    // SAFETY: the caller guarantees validity of a non-null engine.
    let Some(engine) = (unsafe { engine.as_ref() }) else {
        return BridgeStatus::InvalidArgument;
    };
    let Some(resolver) = resolver else {
        return BridgeStatus::InvalidArgument;
    };
    // Read the configuration before decoding: the resolver calls back into the
    // host, so the store lock must not be held while it runs.
    let tuning: Tuning = match engine.store.lock() {
        Ok(store) => *store.tuning(),
        Err(_) => return BridgeStatus::Unavailable,
    };
    // SAFETY: caller guarantees a readable buffer of exactly `length` bytes.
    let bytes = unsafe { slice::from_raw_parts(data, length) };
    let decoded = persistence::deserialize_with_tuning(bytes, tuning, |old_form_id| {
        let mut resolved = 0_u32;
        // SAFETY: the host guarantees callback/context validity for this call.
        let ok = unsafe { resolver(resolver_context, old_form_id, &mut resolved) };
        ok.then_some(resolved)
    });
    let (restored, stats) = match decoded {
        Ok(value) => value,
        Err(PersistenceError::InvalidData) => return BridgeStatus::InvalidData,
        Err(PersistenceError::UnsupportedVersion) => return BridgeStatus::UnsupportedVersion,
    };
    let Ok(mut store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    store.replace(restored);
    // SAFETY: output points to one exclusively writable PersistenceStats.
    unsafe {
        output.write(PersistenceStats {
            loaded: stats.loaded,
            unresolved: stats.unresolved,
        })
    };
    BridgeStatus::Ok
}

/// Clears session state (new game, revert, or before loading persisted data).
///
/// # Safety
/// A non-null engine must be a live context for the duration of this call.
#[no_mangle]
pub unsafe extern "C" fn skyrim_remember_clear(engine: *const MemoryEngine) -> BridgeStatus {
    // SAFETY: the caller guarantees validity of a non-null engine.
    let Some(engine) = (unsafe { engine.as_ref() }) else {
        return BridgeStatus::InvalidArgument;
    };
    let Ok(mut store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    store.clear();
    BridgeStatus::Ok
}

#[cfg(test)]
mod tests;
