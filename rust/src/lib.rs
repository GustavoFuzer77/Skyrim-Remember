use std::ffi::{c_char, CStr};

mod bridge;
mod memory;
mod persistence;

pub use bridge::*;

pub use memory::{DecayHalfLives, MemoryStore, NpcId, NpcMemory};

pub const ABI_VERSION: u32 = 3;
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
