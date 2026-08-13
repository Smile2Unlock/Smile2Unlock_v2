//! Smile2Unlock Windows Credential Provider — Rust implementation.
//!
//! Scope discipline (docs/windows_credential_provider_rust_plan.md):
//! - The DLL is a thin COM adapter: credential serialization, field plumbing,
//!   and an auth-service pipe client only.
//! - No GUI, no camera, no recognizer, no async runtime inside LogonUI.
//!
//! Phase 0: buildable cdylib with the four canonical exports, pure-memory
//! COM tests (GUID/HRESULT/field layout), and the WindowsSecret prototype.
//! COM interface implementations land in Phase 1.

mod fields;
mod secret_buffer;

use windows_core::HRESULT;

pub use fields::{FieldId, FieldState, FieldStatePair};
pub use secret_buffer::{CapacityError, WindowsSecret};

/// Placeholder until Phase 1 wires the class factory. LogonUI gets
/// E_NOTIMPL so the provider never enumerates tiles prematurely.
#[unsafe(no_mangle)]
pub extern "system" fn DllGetClassObject(
    _rclsid: *const windows_core::GUID,
    _riid: *const windows_core::GUID,
    _ppv: *mut *mut core::ffi::c_void,
) -> HRESULT {
    HRESULT::from_win32(0x80004001) // E_NOTIMPL
}

/// No live class factory objects until Phase 1, so the DLL can always unload.
#[unsafe(no_mangle)]
pub extern "system" fn DllCanUnloadNow() -> HRESULT {
    HRESULT(0) // S_OK
}

/// Registry registration arrives in Phase 5 (install/upgrade switching).
#[unsafe(no_mangle)]
pub extern "system" fn DllRegisterServer() -> HRESULT {
    HRESULT::from_win32(0x80004001) // E_NOTIMPL
}

/// Registry unregistration arrives in Phase 5.
#[unsafe(no_mangle)]
pub extern "system" fn DllUnregisterServer() -> HRESULT {
    HRESULT::from_win32(0x80004001) // E_NOTIMPL
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn canonical_clsid_matches_cpp_baseline() {
        let clsid = windows_core::GUID::from(0x5fd3d285_0dd9_4362_8855_e0abaacd4af6u128);
        assert_eq!(clsid.data1, 0x5fd3d285);
        assert_eq!(clsid.data2, 0x0dd9);
        assert_eq!(clsid.data3, 0x4362);
        assert_eq!(clsid.data4, [0x88, 0x55, 0xe0, 0xab, 0xaa, 0xcd, 0x4a, 0xf6]);
    }

    #[test]
    fn hresult_codes_are_stable() {
        assert_eq!(HRESULT::from_win32(0x80004001).0, 0x80004001u32 as i32);
        assert_eq!(HRESULT(0).0, 0);
        assert!(HRESULT::from_win32(0x80004001).is_err());
        assert!(HRESULT(0).is_ok());
    }

    #[test]
    fn field_layout_is_stable() {
        let pairs = fields::state_pairs();
        assert_eq!(pairs.len(), fields::FIELD_COUNT);
        // Tile image is the only logo-field shown in both tiles.
        assert_eq!(pairs[FieldId::TileImage as usize].state, FieldState::DisplayInBoth);
        // Phase 1 finalizes the tile; nothing is focused yet.
        for pair in pairs {
            assert!(pair.focused_index.is_none());
        }
    }
}
