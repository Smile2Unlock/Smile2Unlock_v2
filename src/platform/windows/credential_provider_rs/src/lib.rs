//! Smile2Unlock Windows Credential Provider — Rust implementation.
//!
//! Scope discipline (docs/windows_credential_provider_rust_plan.md):
//! - The DLL is a thin COM adapter: credential serialization, field plumbing,
//!   and an auth-service pipe client only.
//! - No GUI, no camera, no recognizer, no async runtime inside LogonUI.
//!
//! Phase 0: buildable cdylib with the four canonical exports, pure-memory
//! COM tests (GUID/HRESULT/field layout), and the WindowsSecret prototype.
//! Phase 1: DllGetClassObject + class factory + provider + empty credential
//! tile, verified against a pure-memory COM test host.

mod fields;
mod secret_buffer;

#[cfg(windows)]
mod class_factory;
#[cfg(windows)]
mod credential;
#[cfg(windows)]
mod event_sink;
#[cfg(windows)]
mod pipe_client;
#[cfg(windows)]
mod provider;

pub use fields::{FieldId, FieldState, FieldStatePair};
pub use secret_buffer::{CapacityError, WindowsSecret};
#[cfg(windows)]
pub use pipe_client::{current_user_sid, PipeClient, PreparedPipePassword};

use windows_core::HRESULT;

/// ABI-stable HRESULT values (windows-core 0.62 does not re-export these).
pub(crate) const E_NOTIMPL: HRESULT = HRESULT(0x80004001u32 as i32);
pub(crate) const E_NOINTERFACE: HRESULT = HRESULT(0x80004002u32 as i32);
pub(crate) const E_POINTER: HRESULT = HRESULT(0x80004003u32 as i32);
pub(crate) const E_OUTOFMEMORY: HRESULT = HRESULT(0x8007000eu32 as i32);
pub(crate) const E_INVALIDARG: HRESULT = HRESULT(0x80070057u32 as i32);


/// CLSID of the legacy C++ provider ({5fd3d285-0dd9-4362-8855-e0abaacd4af6}).
/// Phase 5 decides between reusing it and switching to a new CLSID; the Rust
/// build must keep the same value until then so LogonUI finds the tile.
#[cfg(windows)]
const CLSID_SU_PROVIDER: windows_core::GUID = windows_core::GUID {
    data1: 0x5fd3d285,
    data2: 0x0dd9,
    data3: 0x4362,
    data4: [0x88, 0x55, 0xe0, 0xab, 0xaa, 0xcd, 0x4a, 0xf6],
};

#[cfg(windows)]
const CLASS_E_CLASSNOTAVAILABLE: HRESULT = HRESULT(0x80040111u32 as i32);

#[cfg(windows)]
use windows::Win32::System::Com::IClassFactory;
#[cfg(windows)]
use windows_core::{IUnknown, Interface};

#[cfg(windows)]
#[unsafe(no_mangle)]
pub extern "system" fn DllGetClassObject(
    rclsid: *const windows_core::GUID,
    riid: *const windows_core::GUID,
    ppv: *mut *mut core::ffi::c_void,
) -> HRESULT {
    if ppv.is_null() {
        return E_POINTER;
    }
    unsafe {
        *ppv = core::ptr::null_mut();
    }
    if rclsid.is_null() || riid.is_null() {
        return E_POINTER;
    }
    // SAFETY: caller passes valid in-parameters per COM contract.
    let clsid = unsafe { *rclsid };
    let requested = unsafe { *riid };
    if clsid != CLSID_SU_PROVIDER {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    if requested != IClassFactory::IID && requested != IUnknown::IID {
        return E_NOINTERFACE;
    }
    let factory: IClassFactory = class_factory::ClassFactory::new().into();
    // SAFETY: into_raw hands over ownership; the caller (COM) releases it.
    let ptr: *mut core::ffi::c_void = factory.into_raw();
    unsafe {
        *ppv = ptr;
    }
    HRESULT(0) // S_OK
}

#[cfg(windows)]
#[unsafe(no_mangle)]
pub extern "system" fn DllCanUnloadNow() -> HRESULT {
    if class_factory::LOCK_COUNT.load(core::sync::atomic::Ordering::SeqCst) > 0 {
        HRESULT(1) // S_FALSE: still locked
    } else {
        HRESULT(0) // S_OK
    }
}

#[cfg(not(windows))]
#[unsafe(no_mangle)]
pub extern "system" fn DllGetClassObject(
    _rclsid: *const windows_core::GUID,
    _riid: *const windows_core::GUID,
    _ppv: *mut *mut core::ffi::c_void,
) -> HRESULT {
    E_NOTIMPL
}

#[cfg(not(windows))]
#[unsafe(no_mangle)]
pub extern "system" fn DllCanUnloadNow() -> HRESULT {
    HRESULT(0) // S_OK
}

/// Registry registration arrives in Phase 5 (install/upgrade switching).
#[unsafe(no_mangle)]
pub extern "system" fn DllRegisterServer() -> HRESULT {
    E_NOTIMPL
}

/// Registry unregistration arrives in Phase 5.
#[unsafe(no_mangle)]
pub extern "system" fn DllUnregisterServer() -> HRESULT {
    E_NOTIMPL
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

    #[test]
    fn field_metadata_is_consistent() {
        use crate::fields::FieldType;
        // Logo/label fields carry their CPFG guids; text/submit fields are
        // zeroed so LogonUI treats them as plain fields.
        assert_ne!(fields::field_type_guid(FieldId::TileImage), (0, 0, 0, [0; 8]));
        assert_ne!(fields::field_type_guid(FieldId::LargeText), (0, 0, 0, [0; 8]));
        assert_eq!(fields::field_type_guid(FieldId::FaceStatus), (0, 0, 0, [0; 8]));
        assert_eq!(fields::field_type_guid(FieldId::SubmitButton), (0, 0, 0, [0; 8]));
        assert_eq!(fields::field_type(FieldId::TileImage), FieldType::TileImage);
        assert_eq!(fields::field_type(FieldId::LargeText), FieldType::LargeText);
        assert_eq!(fields::field_type(FieldId::FaceStatus), FieldType::SmallText);
        assert_eq!(fields::field_type(FieldId::SubmitButton), FieldType::SubmitButton);
        for id in [FieldId::TileImage, FieldId::LargeText, FieldId::FaceStatus, FieldId::SubmitButton] {
            assert!(!fields::label(id).is_empty());
        }
    }
}

#[cfg(all(test, windows))]
mod com_tests {
    use super::*;
    use windows::Win32::UI::Shell::ICredentialProvider;
    use windows::Win32::System::Com::{CoTaskMemFree, IClassFactory};
    use windows_core::BOOL;

    fn call_dll_get_class_object(
        clsid: &windows_core::GUID,
        riid: &windows_core::GUID,
    ) -> (HRESULT, *mut core::ffi::c_void) {
        let mut ppv: *mut core::ffi::c_void = core::ptr::null_mut();
        let hr = DllGetClassObject(clsid, riid, &mut ppv);
        (hr, ppv)
    }

    fn factory_from_clsid() -> IClassFactory {
        let (hr, ppv) = call_dll_get_class_object(&CLSID_SU_PROVIDER, &IClassFactory::IID);
        assert_eq!(hr, HRESULT(0));
        assert!(!ppv.is_null());
        // SAFETY: DllGetClassObject returned a valid IClassFactory pointer.
        unsafe { IClassFactory::from_raw(ppv as *mut _) }
    }

    #[test]
    fn com_activation_success_path() {
        let factory = factory_from_clsid();
        // CreateInstance::<_, ICredentialProvider> with no outer unknown.
        let provider: ICredentialProvider = unsafe {
            factory
                .CreateInstance::<_, ICredentialProvider>(None::<&windows_core::IUnknown>)
                .expect("CreateInstance should succeed")
        };
        let count = unsafe { provider.GetFieldDescriptorCount() }
            .expect("GetFieldDescriptorCount should succeed");
        assert_eq!(count, fields::FIELD_COUNT as u32);
        let mut dwcount: u32 = 0;
        let mut dwdefault: u32 = 0;
        let mut bautologon: BOOL = BOOL(1);
        unsafe { provider.GetCredentialCount(&mut dwcount, &mut dwdefault, &mut bautologon) }
            .expect("GetCredentialCount should succeed");
        assert_eq!(dwcount, 1);
        assert_eq!(dwdefault, 0);
        assert_eq!(bautologon, BOOL(0));
        // Factory is released when `factory` drops.
    }

    #[test]
    fn com_wrong_clsid_fails() {
        let other = windows_core::GUID::from(0x11111111_2222_3333_4444_555555555555u128);
        let (hr, ppv) = call_dll_get_class_object(&other, &IClassFactory::IID);
        assert_eq!(hr.0, 0x80040111u32 as i32); // CLASS_E_CLASSNOTAVAILABLE
        assert!(ppv.is_null());
    }

    #[test]
    fn com_wrong_riid_fails() {
        let (hr, ppv) = call_dll_get_class_object(&CLSID_SU_PROVIDER, &windows_core::GUID::zeroed());
        assert_eq!(hr.0, 0x80004002u32 as i32); // E_NOINTERFACE
        assert!(ppv.is_null());
    }

    #[test]
    fn com_null_output_pointer_fails() {
        let hr = DllGetClassObject(&CLSID_SU_PROVIDER, &IClassFactory::IID, core::ptr::null_mut());
        assert_eq!(hr.0, 0x80004003u32 as i32); // E_POINTER
    }

    #[test]
    fn com_aggregation_rejected() {
        let factory = factory_from_clsid();
        // A real interface instance (any implementor) as the outer unknown.
        let outer: windows_core::IUnknown = crate::provider::Provider::new().into();
        let result: windows_core::Result<ICredentialProvider> = unsafe {
            factory.CreateInstance::<_, ICredentialProvider>(Some(&outer))
        };
        assert_eq!(
            result.err().unwrap().code().0,
            0x80040110u32 as i32 // CLASS_E_NOAGGREGATION
        );
    }

    #[test]
    fn com_field_descriptor_layout() {
        let factory = factory_from_clsid();
        let provider: ICredentialProvider = unsafe {
            factory
                .CreateInstance::<_, ICredentialProvider>(None::<&windows_core::IUnknown>)
                .expect("CreateInstance should succeed")
        };
        // SAFETY: returned pointer is a CoTaskMemAlloc'd descriptor.
        let desc = unsafe { provider.GetFieldDescriptorAt(0) }.expect("field 0 should exist");
        unsafe {
            let d = &*desc;
            assert_eq!(d.dwFieldID, FieldId::TileImage as u32);
            assert_eq!(d.cpft.0, fields::FieldType::TileImage as i32);
            // CPFG_CREDENTIAL_PROVIDER_LOGO
            assert_eq!(d.guidFieldType.data1, 0x2d837775);
            assert!(!d.pszLabel.as_ptr().is_null());
            let mut units = 0usize;
            let mut p = d.pszLabel.as_ptr();
            while *p != 0 {
                units += 1;
                p = p.add(1);
            }
            assert!(units > 0);
            CoTaskMemFree(Some(desc as *const core::ffi::c_void));
        }
    }

    #[test]
    fn com_usage_scenario_gate() {
        use windows::Win32::UI::Shell::{CREDENTIAL_PROVIDER_USAGE_SCENARIO, CPUS_CREDUI, CPUS_LOGON};
        let factory = factory_from_clsid();
        let provider: ICredentialProvider = unsafe {
            factory
                .CreateInstance::<_, ICredentialProvider>(None::<&windows_core::IUnknown>)
                .expect("CreateInstance should succeed")
        };
        unsafe { provider.SetUsageScenario(CPUS_LOGON, 0) }
            .expect("CPUS_LOGON must be accepted");
        let rejected = unsafe { provider.SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO(CPUS_CREDUI.0), 0) };
        assert_eq!(rejected.err().unwrap().code().0, 0x80004001u32 as i32); // E_NOTIMPL
    }

    #[test]
    fn com_lockserver_blocks_unload() {
        let factory = factory_from_clsid();
        unsafe { factory.LockServer(true) }.expect("LockServer(TRUE) should succeed");
        assert_eq!(DllCanUnloadNow(), HRESULT(1)); // S_FALSE
        unsafe { factory.LockServer(false) }.expect("LockServer(FALSE) should succeed");
        assert_eq!(DllCanUnloadNow(), HRESULT(0)); // S_OK
    }
}
