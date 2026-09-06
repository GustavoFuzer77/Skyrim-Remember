//! Tests for the crate-level C entry points. Everything else is tested next to
//! the module that owns it, in `src/<module>/tests.rs`.

use super::*;
use std::sync::atomic::{AtomicBool, Ordering};

static CALLBACK_CALLED: AtomicBool = AtomicBool::new(false);

unsafe extern "C" fn test_logger(message: *const c_char) {
    assert!(!message.is_null());

    // SAFETY: the bridge contract requires a valid NUL-terminated string.
    let message = unsafe { CStr::from_ptr(message) };
    assert_eq!(message, c"Rust bridge initialized");
    CALLBACK_CALLED.store(true, Ordering::SeqCst);
}

#[test]
fn exposes_expected_abi_version() {
    assert_eq!(skyrim_remember_abi_version(), ABI_VERSION);
}

#[test]
fn rejects_a_missing_log_callback() {
    // SAFETY: no callback is invoked in this test.
    assert!(!unsafe { skyrim_remember_initialize(None) });
}

#[test]
fn initializes_through_the_c_abi() {
    CALLBACK_CALLED.store(false, Ordering::SeqCst);
    // SAFETY: test_logger accepts the static C string for the duration of the call.
    assert!(unsafe { skyrim_remember_initialize(Some(test_logger)) });
    assert!(CALLBACK_CALLED.load(Ordering::SeqCst));
}
