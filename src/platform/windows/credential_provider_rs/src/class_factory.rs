//! IClassFactory implementation (Phase 1).
//!
//! CreateInstance hands out a fresh `Provider` and rejects aggregation
//! (CLASS_E_NOAGGREGATION): the provider is a standalone object, never an
//! aggregator. LockServer keeps a process-wide lock counter so the DLL only
//! unloads while nothing is holding a factory.

use core::ffi::c_void;
use core::sync::atomic::{AtomicI32, Ordering};

use windows::Win32::System::Com::{IClassFactory, IClassFactory_Impl};
use windows::Win32::UI::Shell::ICredentialProvider;
use windows_core::{BOOL, Error, GUID, HRESULT, IUnknown, Interface, Ref, implement};

use crate::provider::Provider;
/// Process-wide lock count driven by IClassFactory::LockServer; consumed by
/// DllCanUnloadNow in lib.rs.
pub static LOCK_COUNT: AtomicI32 = AtomicI32::new(0);

// CLASS_E_NOAGGREGATION is not exposed by windows-core; keep the ABI-stable
// value local so Win32_System_Com feature surface stays minimal.
const CLASS_E_NOAGGREGATION: HRESULT = HRESULT(0x80040110u32 as i32);

#[implement(IClassFactory)]
pub struct ClassFactory;

impl ClassFactory {
    pub fn new() -> Self {
        Self
    }
}

impl IClassFactory_Impl for ClassFactory_Impl {
    fn CreateInstance(
        &self,
        punkouter: Ref<IUnknown>,
        riid: *const GUID,
        ppvobject: *mut *mut c_void,
    ) -> Result<(), Error> {
        crate::log::cp_log("ClassFactory::CreateInstance enter");
        if ppvobject.is_null() {
            crate::log::cp_log("ClassFactory::CreateInstance: ppv null -> E_POINTER");
            return Err(Error::from_hresult(crate::E_POINTER));
        }
        unsafe {
            *ppvobject = core::ptr::null_mut();
        }
        if !punkouter.is_null() {
            // No aggregation support: LogonUI never aggregates providers.
            crate::log::cp_log("ClassFactory::CreateInstance: outer -> CLASS_E_NOAGGREGATION");
            return Err(Error::from_hresult(CLASS_E_NOAGGREGATION));
        }
        if riid.is_null() {
            crate::log::cp_log("ClassFactory::CreateInstance: riid null -> E_POINTER");
            return Err(Error::from_hresult(crate::E_POINTER));
        }
        let requested = unsafe { *riid };
        // LogonUI asks for ICredentialProvider; IUnknown::IID is the COM
        // convention fallback. Anything else is E_NOINTERFACE.
        if requested != ICredentialProvider::IID && requested != IUnknown::IID {
            crate::log::cp_log("ClassFactory::CreateInstance: riid mismatch -> E_NOINTERFACE");
            return Err(Error::from_hresult(crate::E_NOINTERFACE));
        }
        crate::log::cp_log("ClassFactory::CreateInstance: handing out Provider");
        let provider: ICredentialProvider = Provider::new().into();
        // SAFETY: into_raw hands over ownership of the refcount; the caller
        // (COM) releases it.
        let ptr: *mut c_void = provider.into_raw();
        unsafe {
            *ppvobject = ptr;
        }
        crate::log::cp_log("ClassFactory::CreateInstance: ok");
        Ok(())
    }

    fn LockServer(&self, flock: BOOL) -> Result<(), Error> {
        if flock.as_bool() {
            LOCK_COUNT.fetch_add(1, Ordering::SeqCst);
        } else {
            let prev = LOCK_COUNT.fetch_sub(1, Ordering::SeqCst);
            if prev <= 0 {
                // fail-secure: never let the counter go negative
                LOCK_COUNT.store(0, Ordering::SeqCst);
            }
        }
        Ok(())
    }
}
