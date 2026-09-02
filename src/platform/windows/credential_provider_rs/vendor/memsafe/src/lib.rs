use error::MemoryError;

mod cell;
pub mod error;
mod ffi;
mod mem_safe;
mod ptr_ops;
mod secret;
#[cfg(feature = "type-state")]
pub mod type_state;

pub use mem_safe::{MemSafe, MemSafeRead, MemSafeWrite};
pub use secret::Secret;

// Project production-gate tests (docs/windows_credential_provider_rust_plan.md):
// - Windows idle seal is PAGE_NOACCESS, elevated only during read/write
//   guards (dump/reading protection).
// - Panic during construction rolls back the protected page cleanly
//   (exception unwinding).
// - Repeated lock/unlock cycles neither leak nor panic (lock-page quota
//   hygiene; exhausting the actual quota is exercised on the real VM).
#[cfg(all(test, windows))]
mod project_tests {
    use core::mem::size_of_val;

    use super::*;
    use winapi::um::memoryapi::VirtualQuery;
    use winapi::um::winnt::{MEMORY_BASIC_INFORMATION, PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE};

    fn protection_of(ptr: *const u8) -> u32 {
        let mut mbi: MEMORY_BASIC_INFORMATION = unsafe { core::mem::zeroed() };
        let n = unsafe { VirtualQuery(ptr as *const _, &mut mbi, size_of_val(&mbi)) };
        assert!(n > 0, "VirtualQuery failed");
        mbi.Protect
    }

    #[test]
    fn windows_idle_seal_is_page_noaccess() {
        let mut secret = Secret::<64>::new_with(|page| page.fill(0x41)).unwrap();
        let ptr = secret.cell_ptr();

        // Idle: sealed, cannot be read (or dumped) without elevation.
        assert_eq!(protection_of(ptr), PAGE_NOACCESS);

        {
            let view = secret.read().unwrap();
            assert_eq!(protection_of(ptr), PAGE_READONLY);
            assert_eq!(*view, [0x41; 64]);
        }
        // Guard dropped: sealed again.
        assert_eq!(protection_of(ptr), PAGE_NOACCESS);

        {
            let mut view = secret.write().unwrap();
            assert_eq!(protection_of(ptr), PAGE_READWRITE);
            view.fill(0x42);
        }
        assert_eq!(protection_of(ptr), PAGE_NOACCESS);

        // Stale-pointer hygiene: a fresh read sees the new content and the
        // same sealed state (the old guard's pointer is unusable by borrow
        // rules; the page state must still round-trip).
        let view = secret.read().unwrap();
        assert_eq!(*view, [0x42; 64]);
        assert_eq!(protection_of(ptr), PAGE_READONLY);
        drop(view);
        assert_eq!(protection_of(ptr), PAGE_NOACCESS);
    }

    #[test]
    fn init_panic_rolls_back_cleanly() {
        // Panic inside the init closure must not abort (Drop-based rollback
        // wipes the page, unlocks and unmaps) and must not poison later use.
        let result = std::panic::catch_unwind(|| {
            let _ = Secret::<64>::new_with(|_| panic!("init panic"));
        });
        assert!(result.is_err(), "panic must propagate");

        // A fresh secret still works after the rollback.
        let mut secret = Secret::<64>::new_with(|page| page.fill(7)).unwrap();
        let view = secret.read().unwrap();
        assert_eq!(*view, [7; 64]);
    }

    #[test]
    fn lock_cycle_does_not_leak_or_panic() {
        // Locked pages are released on drop; repeated create/write/drop
        // cycles must not exhaust the lock-page quota or panic. (Quota
        // exhaustion itself is exercised on the real Windows VM.)
        for _ in 0..256 {
            let mut secret = Secret::<256>::new_with(|page| page.fill(0x5a)).unwrap();
            {
                let mut view = secret.write().unwrap();
                view.fill(0x00);
            }
            drop(secret);
        }
    }
}
