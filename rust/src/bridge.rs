use crate::persistence::{self, PersistenceError};
use crate::{DecayHalfLives, MemoryStore, NpcId, NpcMemory};
use std::ffi::c_void;
use std::slice;
use std::sync::Mutex;

/// Opaque to C++. Allocate and release only through this module's C API.
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
#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct MemorySnapshot {
    pub recognition: f32,
    pub suspicion: f32,
    pub fear: f32,
    pub respect: f32,
    pub last_decay_game_days: f64,
}

impl From<NpcMemory> for MemorySnapshot {
    fn from(memory: NpcMemory) -> Self {
        Self {
            recognition: memory.recognition,
            suspicion: memory.suspicion,
            fear: memory.fear,
            respect: memory.respect,
            last_decay_game_days: memory.last_decay_game_days,
        }
    }
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct MemoryDelta {
    pub recognition: f32,
    pub suspicion: f32,
    pub fear: f32,
    pub respect: f32,
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct PersistenceStats {
    pub loaded: u32,
    pub unresolved: u32,
}

pub type FormIdResolver =
    unsafe extern "C" fn(context: *mut c_void, old_form_id: u32, new_form_id: *mut u32) -> bool;

fn valid_game_days(game_days: f64) -> bool {
    game_days.is_finite() && game_days >= 0.0
}

fn valid_delta(delta: MemoryDelta) -> bool {
    delta.recognition.is_finite()
        && delta.suspicion.is_finite()
        && delta.fear.is_finite()
        && delta.respect.is_finite()
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
    if let Some(memory) = store.get_mut(id) {
        memory.apply_decay(game_days, DecayHalfLives::default());
        return BridgeStatus::Ok;
    }
    store.get_or_create_at(id, game_days);
    BridgeStatus::Created
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
    let Some(memory) = store.get_mut(id) else {
        return BridgeStatus::NotFound;
    };
    memory.apply_decay(game_days, DecayHalfLives::default());
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
    let memory = store.get_or_create_at(id, game_days);
    memory.apply_decay(game_days, DecayHalfLives::default());
    memory.add(
        delta.recognition,
        delta.suspicion,
        delta.fear,
        delta.respect,
    );
    // SAFETY: caller owns writable output storage; the snapshot has no pointers.
    unsafe { output.write((*memory).into()) };
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
    if written.is_null() {
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
        return if capacity == 0 {
            BridgeStatus::Ok
        } else {
            BridgeStatus::InvalidArgument
        };
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
    // SAFETY: caller guarantees a readable buffer of exactly `length` bytes.
    let bytes = unsafe { slice::from_raw_parts(data, length) };
    let decoded = persistence::deserialize_with(bytes, |old_form_id| {
        let mut resolved = 0_u32;
        // SAFETY: the host guarantees callback/context validity for this call.
        let ok = unsafe { resolver(resolver_context, old_form_id, &mut resolved) };
        ok.then_some(resolved)
    });
    let (memories, stats) = match decoded {
        Ok(value) => value,
        Err(PersistenceError::InvalidData) => return BridgeStatus::InvalidData,
        Err(PersistenceError::UnsupportedVersion) => return BridgeStatus::UnsupportedVersion,
    };
    let Ok(mut store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    store.replace(memories);
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
