use crate::{MemoryStore, NpcId, NpcMemory};
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
}

/// Copied into caller-owned storage; no Rust allocation crosses the ABI.
#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct MemorySnapshot {
    pub recognition: f32,
    pub suspicion: f32,
    pub fear: f32,
    pub respect: f32,
}

impl From<NpcMemory> for MemorySnapshot {
    fn from(memory: NpcMemory) -> Self {
        Self {
            recognition: memory.recognition,
            suspicion: memory.suspicion,
            fear: memory.fear,
            respect: memory.respect,
        }
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
) -> BridgeStatus {
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
    if store.get(id).is_some() {
        return BridgeStatus::Ok;
    }
    store.get_or_create(id);
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
    let Ok(store) = engine.store.lock() else {
        return BridgeStatus::Unavailable;
    };
    let Some(memory) = store.get(id) else {
        return BridgeStatus::NotFound;
    };
    // SAFETY: caller owns writable output storage; the snapshot has no pointers.
    unsafe { output.write((*memory).into()) };
    BridgeStatus::Ok
}

/// Clears session state (new game or load; persistence is not implemented yet).
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
