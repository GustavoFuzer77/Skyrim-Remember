use std::ffi::{c_char, CStr};

mod appearance;
mod bridge;
mod memory;
mod persistence;
mod tuning;

pub use bridge::*;

pub use appearance::{
    appearance_is_valid, outfit_similarity, AppearanceCategory, AppearanceItem,
    AppearanceObservation, KnownAppearance,
};

pub use memory::{MemoryStore, NpcId, NpcMemory};

pub use tuning::{
    AppearanceWeights, DecayHalfLives, Limits, RecognitionTuning, Tuning, MAX_APPEARANCE_ITEMS,
    MAX_KNOWN_APPEARANCES, MAX_MEMORY_VALUE,
};

/// Bumped whenever a signature, struct layout, or field meaning changes.
/// `RustBridge.h` mirrors this as `SkyrimRememberAbiVersion` and `BridgeSmoke`
/// compares the two at run time.
pub const ABI_VERSION: u32 = 7;
const INITIALIZED_MESSAGE: &CStr = c"Rust bridge initialized";

pub type SkyrimRememberLogCallback = unsafe extern "C" fn(message: *const c_char);

#[no_mangle]
pub extern "C" fn skyrim_remember_abi_version() -> u32 {
    ABI_VERSION
}

#[no_mangle]
/// Initializes the Rust core and reports completion through the supplied logger.
///
/// # Safety
///
/// `log_callback`, when present, must point to a valid function that accepts a
/// non-null, NUL-terminated string for the duration of the call. The callback
/// must not retain the string pointer after it returns.
/// It must not unwind, throw an exception, or panic across this ABI boundary.
pub unsafe extern "C" fn skyrim_remember_initialize(
    log_callback: Option<SkyrimRememberLogCallback>,
) -> bool {
    let Some(log) = log_callback else {
        return false;
    };

    // SAFETY: the C++ bridge promises that the callback is valid for this call.
    // INITIALIZED_MESSAGE is static and NUL-terminated.
    unsafe { log(INITIALIZED_MESSAGE.as_ptr()) };
    true
}

#[cfg(test)]
mod tests;
