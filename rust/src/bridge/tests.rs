use super::*;
use std::ptr;

struct Context(*mut MemoryEngine);

impl Context {
    fn new() -> Self {
        Self(skyrim_remember_create())
    }
}

impl Drop for Context {
    fn drop(&mut self) {
        // SAFETY: this test owns the context and no concurrent calls exist.
        unsafe { skyrim_remember_destroy(self.0) };
    }
}

#[test]
fn abi_layout_matches_cpp_contract() {
    assert_eq!(std::mem::size_of::<MemorySnapshot>(), 24);
    assert_eq!(std::mem::align_of::<MemorySnapshot>(), 8);
    assert_eq!(std::mem::offset_of!(MemorySnapshot, recognition), 0);
    assert_eq!(std::mem::offset_of!(MemorySnapshot, suspicion), 4);
    assert_eq!(std::mem::offset_of!(MemorySnapshot, fear), 8);
    assert_eq!(std::mem::offset_of!(MemorySnapshot, respect), 12);
    assert_eq!(
        std::mem::offset_of!(MemorySnapshot, last_decay_game_days),
        16
    );
    assert_eq!(std::mem::size_of::<MemoryDelta>(), 16);
    assert_eq!(std::mem::size_of::<PersistenceStats>(), 8);
    assert_eq!(std::mem::size_of::<BridgeStatus>(), 4);
}

#[test]
fn query_does_not_register_and_preserves_output_on_error() {
    let context = Context::new();
    let sentinel = MemorySnapshot {
        recognition: 7.0,
        ..Default::default()
    };
    let mut snapshot = sentinel;
    // SAFETY: valid live context and exclusively owned output.
    unsafe {
        assert_eq!(
            skyrim_remember_get(context.0, 123, 10.0, &mut snapshot),
            BridgeStatus::NotFound
        );
        assert_eq!(snapshot, sentinel);
        assert_eq!(
            skyrim_remember_register(context.0, 123, 10.0),
            BridgeStatus::Created
        );
        assert_eq!(
            skyrim_remember_get(context.0, 123, 10.0, &mut snapshot),
            BridgeStatus::Ok
        );
    }
    assert_eq!(
        snapshot,
        MemorySnapshot {
            last_decay_game_days: 10.0,
            ..MemorySnapshot::default()
        }
    );
}

unsafe extern "C" fn remap_form(
    _context: *mut std::ffi::c_void,
    old_form_id: u32,
    new_form_id: *mut u32,
) -> bool {
    if old_form_id == 456 {
        return false;
    }
    // SAFETY: the persistence bridge provides writable storage for one u32.
    unsafe { new_form_id.write(old_form_id + 1000) };
    true
}

#[test]
fn ffi_persistence_round_trip_is_atomic_and_resolves_form_ids() {
    let context = Context::new();
    let mut snapshot = MemorySnapshot::default();
    // SAFETY: context and all stack outputs remain valid throughout the test.
    unsafe {
        assert_eq!(
            skyrim_remember_apply_delta(
                context.0,
                123,
                10.0,
                MemoryDelta {
                    fear: 30.0,
                    respect: 50.0,
                    ..MemoryDelta::default()
                },
                &mut snapshot,
            ),
            BridgeStatus::Ok
        );
        assert_eq!(
            skyrim_remember_register(context.0, 456, 10.0),
            BridgeStatus::Created
        );

        let mut length = 0_usize;
        assert_eq!(
            skyrim_remember_serialize(context.0, ptr::null_mut(), 0, &mut length),
            BridgeStatus::Ok
        );
        let mut bytes = vec![0_u8; length];
        assert_eq!(
            skyrim_remember_serialize(context.0, bytes.as_mut_ptr(), bytes.len(), &mut length),
            BridgeStatus::Ok
        );
        assert_eq!(skyrim_remember_clear(context.0), BridgeStatus::Ok);

        let mut stats = PersistenceStats::default();
        assert_eq!(
            skyrim_remember_deserialize(
                context.0,
                bytes.as_ptr(),
                bytes.len(),
                Some(remap_form),
                ptr::null_mut(),
                &mut stats,
            ),
            BridgeStatus::Ok
        );
        assert_eq!(
            stats,
            PersistenceStats {
                loaded: 1,
                unresolved: 1
            }
        );
        assert_eq!(
            skyrim_remember_get(context.0, 1123, 10.0, &mut snapshot),
            BridgeStatus::Ok
        );
        assert_eq!(snapshot.fear, 30.0);
        assert_eq!(snapshot.respect, 50.0);

        let bad = [1_u8, 2, 3];
        assert_eq!(
            skyrim_remember_deserialize(
                context.0,
                bad.as_ptr(),
                bad.len(),
                Some(remap_form),
                ptr::null_mut(),
                &mut stats,
            ),
            BridgeStatus::InvalidData
        );
        assert_eq!(
            skyrim_remember_get(context.0, 1123, 10.0, &mut snapshot),
            BridgeStatus::Ok
        );
    }
}

#[test]
fn registration_preserves_values_and_actors_are_isolated() {
    let context = Context::new();
    // SAFETY: context is alive for every operation; outputs are stack-owned.
    unsafe {
        assert_eq!(
            skyrim_remember_register(context.0, 123, 10.0),
            BridgeStatus::Created
        );
        assert_eq!(
            skyrim_remember_register(context.0, 456, 10.0),
            BridgeStatus::Created
        );
        // Inject nonzero data only in the test to verify snapshot field ordering.
        *(*context.0)
            .store
            .lock()
            .unwrap()
            .get_or_create(NpcId::new(123).unwrap()) = NpcMemory {
            recognition: 11.0,
            suspicion: 22.0,
            fear: 33.0,
            respect: 44.0,
            last_decay_game_days: 10.0,
        };
        assert_eq!(
            skyrim_remember_register(context.0, 123, 10.0),
            BridgeStatus::Ok
        );
        let mut a = MemorySnapshot::default();
        let mut b = MemorySnapshot::default();
        assert_eq!(
            skyrim_remember_get(context.0, 123, 10.0, &mut a),
            BridgeStatus::Ok
        );
        assert_eq!(
            skyrim_remember_get(context.0, 456, 10.0, &mut b),
            BridgeStatus::Ok
        );
        assert_eq!(
            a,
            MemorySnapshot {
                recognition: 11.0,
                suspicion: 22.0,
                fear: 33.0,
                respect: 44.0,
                last_decay_game_days: 10.0
            }
        );
        assert_eq!(b.last_decay_game_days, 10.0);
        assert_eq!(b.fear, 0.0);
        assert_eq!((*context.0).store.lock().unwrap().len(), 2);
    }
}

#[test]
fn reset_clears_state_and_contexts_are_independent() {
    let a = Context::new();
    let b = Context::new();
    let mut snapshot = MemorySnapshot::default();
    // SAFETY: both contexts and output live throughout the operations.
    unsafe {
        assert_eq!(
            skyrim_remember_register(a.0, 123, 1.0),
            BridgeStatus::Created
        );
        assert_eq!(
            skyrim_remember_get(b.0, 123, 1.0, &mut snapshot),
            BridgeStatus::NotFound
        );
        assert_eq!(
            skyrim_remember_register(b.0, 456, 1.0),
            BridgeStatus::Created
        );
        assert_eq!(skyrim_remember_clear(a.0), BridgeStatus::Ok);
        assert_eq!(
            skyrim_remember_get(a.0, 123, 1.0, &mut snapshot),
            BridgeStatus::NotFound
        );
        assert_eq!(
            skyrim_remember_get(b.0, 456, 1.0, &mut snapshot),
            BridgeStatus::Ok
        );
        assert_eq!(
            skyrim_remember_register(a.0, 123, 1.0),
            BridgeStatus::Created
        );
    }
}

#[test]
fn rejects_null_arguments_and_zero_ids_without_mutation() {
    let context = Context::new();
    let mut snapshot = MemorySnapshot {
        fear: 7.0,
        ..Default::default()
    };
    // SAFETY: null arguments are explicitly supported; other pointers are valid.
    unsafe {
        skyrim_remember_destroy(ptr::null_mut());
        assert_eq!(
            skyrim_remember_register(ptr::null(), 123, 1.0),
            BridgeStatus::InvalidArgument
        );
        assert_eq!(
            skyrim_remember_clear(ptr::null()),
            BridgeStatus::InvalidArgument
        );
        assert_eq!(
            skyrim_remember_get(ptr::null(), 123, 1.0, &mut snapshot),
            BridgeStatus::InvalidArgument
        );
        assert_eq!(
            skyrim_remember_get(context.0, 123, 1.0, ptr::null_mut()),
            BridgeStatus::InvalidArgument
        );
        assert_eq!(
            skyrim_remember_register(context.0, 0, 1.0),
            BridgeStatus::InvalidId
        );
        assert_eq!(
            skyrim_remember_get(context.0, 0, 1.0, &mut snapshot),
            BridgeStatus::InvalidId
        );
        assert!((*context.0).store.lock().unwrap().is_empty());
    }
    assert_eq!(snapshot.fear, 7.0);
}

#[test]
fn poisoned_lock_returns_an_error_without_unwinding_over_ffi() {
    let context = Context::new();
    // SAFETY: the context remains alive and exclusively owned in this test.
    let engine = unsafe { &*context.0 };
    let result = std::panic::catch_unwind(|| {
        let _lock = engine.store.lock().unwrap();
        panic!("poison the lock in ordinary Rust, outside any extern C call");
    });
    assert!(result.is_err());
    let mut snapshot = MemorySnapshot::default();
    // SAFETY: live context, valid output; poisoning does not invalidate pointers.
    unsafe {
        assert_eq!(
            skyrim_remember_register(context.0, 123, 1.0),
            BridgeStatus::Unavailable
        );
        assert_eq!(
            skyrim_remember_get(context.0, 123, 1.0, &mut snapshot),
            BridgeStatus::Unavailable
        );
        assert_eq!(skyrim_remember_clear(context.0), BridgeStatus::Unavailable);
    }
}
