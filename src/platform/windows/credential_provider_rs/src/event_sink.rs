//! `ICredentialProviderEvents` notification through the COM Global Interface
//! Table (GIT).
//!
//! `Provider::Advise` receives the events interface on LogonUI's apartment and
//! registers it in the process-wide GIT. The recognition worker runs in an MTA
//! and obtains a marshaled proxy with `GetInterfaceFromGlobal` before calling
//! `CredentialsChanged`. Storing the raw interface pointer across threads is
//! forbidden, and no `Send`/`Sync` bypass is used: the GIT does the marshaling
//! that COM requires.

use windows::Win32::System::Com::{CLSCTX_INPROC_SERVER, CoCreateInstance, IGlobalInterfaceTable};
use windows::Win32::UI::Shell::ICredentialProviderEvents;
use windows_core::{GUID, Interface};

/// `CLSID_StdGlobalInterfaceTable`; the windows crate does not export it.
pub const CLSID_STD_GLOBAL_INTERFACE_TABLE: GUID = GUID {
    data1: 0x0000_0323,
    data2: 0x0000,
    data3: 0x0000,
    data4: [0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46],
};

/// Consumer of the "a grant is ready" notification. The Windows implementation
/// calls `ICredentialProviderEvents::CredentialsChanged` through the GIT; a
/// test implementation counts calls.
pub trait ReadyNotifier: Send + Sync + 'static {
    fn notify_ready(&self);
}

/// GIT-backed notifier. `register` runs on the apartment that received
/// `Provider::Advise`; `notify_ready` runs on the worker thread.
pub struct GitEventNotifier {
    cookie: u32,
    context: usize,
}

impl GitEventNotifier {
    pub fn register(
        events: &ICredentialProviderEvents,
        context: usize,
    ) -> windows_core::Result<Self> {
        let unknown: windows_core::IUnknown = events.cast()?;
        // SAFETY: CoCreateInstance with a documented in-proc singleton; the
        // caller's apartment is COM initialized by LogonUI.
        let git: IGlobalInterfaceTable = unsafe {
            CoCreateInstance(
                &CLSID_STD_GLOBAL_INTERFACE_TABLE,
                None,
                CLSCTX_INPROC_SERVER,
            )?
        };
        // SAFETY: `unknown` is a live interface and the IID is its own.
        let cookie =
            unsafe { git.RegisterInterfaceInGlobal(&unknown, &ICredentialProviderEvents::IID)? };
        Ok(Self { cookie, context })
    }
}

impl ReadyNotifier for GitEventNotifier {
    fn notify_ready(&self) {
        let result = (|| -> windows_core::Result<()> {
            let git: IGlobalInterfaceTable = unsafe {
                CoCreateInstance(
                    &CLSID_STD_GLOBAL_INTERFACE_TABLE,
                    None,
                    CLSCTX_INPROC_SERVER,
                )?
            };
            let mut raw: *mut core::ffi::c_void = core::ptr::null_mut();
            // SAFETY: cookie was registered for ICredentialProviderEvents.
            unsafe {
                git.GetInterfaceFromGlobal(self.cookie, &ICredentialProviderEvents::IID, &mut raw)?;
            }
            if raw.is_null() {
                return Err(windows_core::Error::from_hresult(crate::E_FAIL));
            }
            // SAFETY: GetInterfaceFromGlobal returned an owned interface.
            let events = unsafe { ICredentialProviderEvents::from_raw(raw) };
            // SAFETY: CredentialsChanged is the documented notification.
            unsafe { events.CredentialsChanged(self.context)? };
            Ok(())
        })();
        if let Err(error) = result {
            crate::log::cp_log(&format!("auto notify_ready FAILED {:08x}", error.code().0));
        }
    }
}

impl Drop for GitEventNotifier {
    fn drop(&mut self) {
        let result = (|| -> windows_core::Result<()> {
            let git: IGlobalInterfaceTable = unsafe {
                CoCreateInstance(
                    &CLSID_STD_GLOBAL_INTERFACE_TABLE,
                    None,
                    CLSCTX_INPROC_SERVER,
                )?
            };
            // SAFETY: revoking our own cookie exactly once.
            unsafe { git.RevokeInterfaceFromGlobal(self.cookie)? };
            Ok(())
        })();
        if let Err(error) = result {
            crate::log::cp_log(&format!(
                "auto revoke notifier FAILED {:08x}",
                error.code().0
            ));
        }
    }
}
