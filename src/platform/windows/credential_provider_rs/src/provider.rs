//! ICredentialProvider + ICredentialProviderSetUserArray (Phase 1).
//!
//! The provider exposes the v1 field set (see fields.rs) and hands out a
//! one tile per user. It accepts CPUS_LOGON and CPUS_UNLOCK_WORKSTATION
//! only; all other scenarios (including CPUS_CREDUI) are E_NOTIMPL so
//! LogonUI never shows this provider in credential-ui flows.
//!
//! Field descriptors are allocated with CoTaskMemAlloc because LogonUI
//! releases them with CoTaskMemFree.

use core::cell::{Cell, RefCell};
use core::ptr;
use std::sync::Arc;

use windows::Win32::System::Com::{CoTaskMemAlloc, CoTaskMemFree};
use windows::Win32::UI::Shell::{
    CPUS_LOGON, CPUS_UNLOCK_WORKSTATION, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR,
    CREDENTIAL_PROVIDER_FIELD_TYPE, CREDENTIAL_PROVIDER_USAGE_SCENARIO, ICredentialProvider,
    ICredentialProvider_Impl, ICredentialProviderCredential, ICredentialProviderEvents,
    ICredentialProviderSetUserArray, ICredentialProviderSetUserArray_Impl,
    ICredentialProviderUserArray,
};
use windows_core::{BOOL, Error, GUID, PWSTR, Ref, implement};

use crate::auto_runtime::{AutoRuntime, MonotonicClock, PipeRecognitionTransport};
use crate::credential::Credential;
use crate::event_sink::GitEventNotifier;
use crate::fields::{self, FieldId};

#[implement(ICredentialProvider, ICredentialProviderSetUserArray)]
pub struct Provider {
    /// CPUS_* value accepted by SetUsageScenario; None until set.
    usage_scenario: Cell<Option<i32>>,
    /// upadvisecontext from the last Advise call.
    advised_context: Cell<usize>,
    /// SIDs passed via SetUserArray. V2 credential providers must return a
    /// valid SID from ICredentialProviderCredential2::GetUserSid for every
    /// tile or LogonUI discards it.
    user_sids: RefCell<Vec<String>>,
    /// Process-wide automatic-recognition worker shared by every tile.
    runtime: Arc<AutoRuntime<PipeRecognitionTransport>>,
}

impl Provider {
    pub fn new() -> Self {
        Self {
            usage_scenario: Cell::new(None),
            advised_context: Cell::new(0),
            user_sids: RefCell::new(Vec::new()),
            runtime: AutoRuntime::new(PipeRecognitionTransport, Box::new(MonotonicClock)),
        }
    }

    fn supported_scenario(cpus: CREDENTIAL_PROVIDER_USAGE_SCENARIO) -> bool {
        cpus == CPUS_LOGON || cpus == CPUS_UNLOCK_WORKSTATION
    }

    fn descriptor_for(index: u32) -> Option<CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR> {
        let id = match index {
            0 => FieldId::TileImage,
            1 => FieldId::Label,
            2 => FieldId::LargeText,
            3 => FieldId::PasswordText,
            4 => FieldId::SubmitButton,
            _ => return None,
        };
        let field_type = CREDENTIAL_PROVIDER_FIELD_TYPE(fields::field_type(id) as i32);
        let (d1, d2, d3, d4) = fields::field_type_guid(id);
        let guid = GUID {
            data1: d1,
            data2: d2,
            data3: d3,
            data4: d4,
        };
        Some(CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR {
            dwFieldID: index,
            cpft: field_type,
            pszLabel: PWSTR::null(),
            guidFieldType: guid,
        })
    }
}

impl ICredentialProvider_Impl for Provider_Impl {
    fn SetUsageScenario(
        &self,
        cpus: CREDENTIAL_PROVIDER_USAGE_SCENARIO,
        _dwflags: u32,
    ) -> windows_core::Result<()> {
        crate::log::cp_log(&format!("Provider::SetUsageScenario({})", cpus.0));
        if !Provider::supported_scenario(cpus) {
            // CREDUI/change-password/remote flows are out of scope for v1.
            crate::log::cp_log("Provider::SetUsageScenario: unsupported -> E_NOTIMPL");
            return Err(Error::from_hresult(crate::E_NOTIMPL));
        }
        self.usage_scenario.set(Some(cpus.0));
        Ok(())
    }

    fn SetSerialization(
        &self,
        _pcpcs: *const windows::Win32::UI::Shell::CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION,
    ) -> windows_core::Result<()> {
        // The credential always comes from the auth-service pipe (Phase 2/3);
        // serialized input from LogonUI is ignored.
        Err(Error::from_hresult(crate::E_NOTIMPL))
    }

    fn Advise(
        &self,
        pcpe: Ref<ICredentialProviderEvents>,
        upadvisecontext: usize,
    ) -> windows_core::Result<()> {
        crate::log::cp_log("Provider::Advise");
        self.advised_context.set(upadvisecontext);
        // Allow a provider instance that was UnAdvise'd to be re-armed.
        self.runtime.arm();
        // Marshal the events interface through the Global Interface Table so
        // the recognition worker (a different apartment) can call
        // CredentialsChanged. Storing the raw pointer across threads is
        // forbidden; the GIT hands out a proxy instead.
        match pcpe.as_ref() {
            Some(events) => match GitEventNotifier::register(events, upadvisecontext) {
                Ok(notifier) => {
                    self.runtime.set_notifier(Arc::new(notifier));
                    crate::log::cp_log("Provider::Advise: events marshalled into GIT");
                }
                Err(error) => crate::log::cp_log(&format!(
                    "Provider::Advise: GIT registration FAILED {:08x}",
                    error.code().0
                )),
            },
            None => crate::log::cp_log("Provider::Advise: events pointer missing"),
        }
        Ok(())
    }

    fn UnAdvise(&self) -> windows_core::Result<()> {
        crate::log::cp_log("Provider::UnAdvise");
        self.advised_context.set(0);
        // Stop the worker and revoke the marshalled interface. The worker
        // exits on its own; the UI thread never joins it.
        self.runtime.shutdown();
        self.runtime.clear_notifier();
        Ok(())
    }

    fn GetFieldDescriptorCount(&self) -> windows_core::Result<u32> {
        crate::log::cp_log("Provider::GetFieldDescriptorCount");
        Ok(fields::FIELD_COUNT as u32)
    }

    fn GetFieldDescriptorAt(
        &self,
        dwindex: u32,
    ) -> windows_core::Result<*mut CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR> {
        crate::log::cp_log(&format!("Provider::GetFieldDescriptorAt({})", dwindex));
        let mut descriptor = match Provider::descriptor_for(dwindex) {
            Some(d) => d,
            None => {
                return Err(Error::from_hresult(crate::E_INVALIDARG));
            }
        };
        let id = match dwindex {
            0 => FieldId::TileImage,
            1 => FieldId::Label,
            2 => FieldId::LargeText,
            3 => FieldId::PasswordText,
            _ => FieldId::SubmitButton,
        };
        // Allocate the label with the COM task allocator; LogonUI releases
        // the whole descriptor (label included) with CoTaskMemFree.
        let label: Vec<u16> = fields::label(id)
            .encode_utf16()
            .chain(core::iter::once(0))
            .collect();
        let label_bytes = label.len() * core::mem::size_of::<u16>();
        // SAFETY: CoTaskMemAlloc returns a writable block of label_bytes.
        let label_mem = unsafe { CoTaskMemAlloc(label_bytes) };
        if label_mem.is_null() {
            return Err(Error::from_hresult(crate::E_OUTOFMEMORY));
        }
        // SAFETY: label_mem holds at least label.len() u16s.
        unsafe {
            ptr::copy_nonoverlapping(label.as_ptr(), label_mem as *mut u16, label.len());
        }
        descriptor.pszLabel = PWSTR(label_mem as *mut u16);

        // The descriptor itself is heap-allocated so LogonUI can CoTaskMemFree
        // it after use.
        let desc_bytes = core::mem::size_of::<CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR>();
        // SAFETY: CoTaskMemAlloc returns a writable block of desc_bytes.
        let desc_mem =
            unsafe { CoTaskMemAlloc(desc_bytes) } as *mut CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR;
        if desc_mem.is_null() {
            // SAFETY: label_mem was allocated with CoTaskMemAlloc.
            unsafe { CoTaskMemFree(Some(label_mem)) };
            return Err(Error::from_hresult(crate::E_OUTOFMEMORY));
        }
        // SAFETY: desc_mem holds exactly one descriptor; caller takes ownership.
        unsafe {
            ptr::write(desc_mem, descriptor);
        }
        Ok(desc_mem)
    }

    fn GetCredentialCount(
        &self,
        pdwcount: *mut u32,
        pdwdefault: *mut u32,
        pbautologonwithdefault: *mut BOOL,
    ) -> windows_core::Result<()> {
        crate::log::cp_log("Provider::GetCredentialCount");
        // When automatic recognition published a grant, LogonUI is woken with
        // CredentialsChanged and re-queries this method: make that tile the
        // default and request autologon so LogonUI calls GetSerialization
        // without a click. The credential still consumes the grant exactly
        // once, and a manual password tile is unaffected.
        let user_sids = self.user_sids.borrow();
        let ready_sid = self.runtime.ready_sid();
        let default_index = ready_sid
            .as_ref()
            .and_then(|sid| user_sids.iter().position(|candidate| candidate == sid))
            .map(|index| index as u32)
            .unwrap_or(0);
        let autologon = ready_sid.is_some();
        let user_count = user_sids.len();
        drop(user_sids);
        if !pdwcount.is_null() {
            unsafe { *pdwcount = user_count as u32 };
        }
        if !pdwdefault.is_null() {
            unsafe { *pdwdefault = default_index };
        }
        if !pbautologonwithdefault.is_null() {
            unsafe { *pbautologonwithdefault = BOOL(autologon as i32) };
        }
        crate::log::cp_log(&format!(
            "Provider::GetCredentialCount autologon={} default={}",
            autologon, default_index
        ));
        Ok(())
    }

    fn GetCredentialAt(&self, dwindex: u32) -> windows_core::Result<ICredentialProviderCredential> {
        crate::log::cp_log(&format!("Provider::GetCredentialAt({})", dwindex));
        let user_sid = self.user_sids.borrow().get(dwindex as usize).cloned();
        if user_sid.is_none() {
            return Err(Error::from_hresult(crate::E_INVALIDARG));
        }
        // No diagnostic method calls here: invoking SetSelected during
        // enumeration would start an automatic recognition attempt before
        // LogonUI has actually selected the tile.
        let credential: ICredentialProviderCredential = Credential::new(
            self.usage_scenario.get().unwrap_or(0),
            user_sid,
            Arc::clone(&self.runtime),
        )
        .into();
        Ok(credential)
    }
}

impl ICredentialProviderSetUserArray_Impl for Provider_Impl {
    fn SetUserArray(&self, users: Ref<ICredentialProviderUserArray>) -> windows_core::Result<()> {
        crate::log::cp_log("Provider::SetUserArray");
        // A new user array invalidates every tile that referenced the old one.
        self.runtime.cancel();
        // V2 credential providers must associate each tile with a valid user
        // SID. Capture every enumerated user so multi-user logon screens get
        // one tile per account.
        self.user_sids.borrow_mut().clear();
        if let Some(array) = users.as_ref()
            && let Ok(count) = unsafe { array.GetCount() }
        {
            crate::log::cp_log(&format!("Provider::SetUserArray count={}", count));
            for index in 0..count {
                if let Ok(user) = unsafe { array.GetAt(index) }
                    && let Ok(sid) = unsafe { user.GetSid() }
                    && let Ok(text) = unsafe { sid.to_string() }
                {
                    crate::log::cp_log(&format!("Provider::SetUserArray sid={}", text));
                    self.user_sids.borrow_mut().push(text);
                }
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::auto_recognition::Phase;

    fn object_with_users(sids: &[&str]) -> windows_core::ComObject<Provider> {
        let object = windows_core::ComObject::new(Provider::new());
        object.usage_scenario.set(Some(CPUS_LOGON.0));
        object
            .user_sids
            .borrow_mut()
            .extend(sids.iter().map(|sid| (*sid).to_owned()));
        object
    }

    #[test]
    fn enumeration_does_not_start_recognition() {
        // GetCredentialAt used to call SetSelected as a diagnostic self-test.
        // In automatic mode that would start a camera attempt during tile
        // enumeration, before LogonUI actually selected the tile.
        let object = object_with_users(&["S-1-5-21-1"]);
        let provider = object.to_interface::<ICredentialProvider>();
        let credential = unsafe { provider.GetCredentialAt(0) }.expect("tile should exist");
        assert_eq!(object.runtime.phase(), Phase::Idle);
        drop(credential);
    }

    #[test]
    fn credential_count_reports_no_autologon_without_grant() {
        let object = object_with_users(&["S-1-5-21-1", "S-1-5-21-2"]);
        let provider = object.to_interface::<ICredentialProvider>();
        let mut count = 0u32;
        let mut default = 99u32;
        let mut autologon = BOOL(1);
        unsafe { provider.GetCredentialCount(&mut count, &mut default, &mut autologon) }
            .expect("count should succeed");
        assert_eq!(count, 2);
        assert_eq!(default, 0);
        assert_eq!(autologon, BOOL(0));
    }

    #[test]
    fn invalid_index_is_rejected() {
        let object = object_with_users(&["S-1-5-21-1"]);
        let provider = object.to_interface::<ICredentialProvider>();
        assert!(unsafe { provider.GetCredentialAt(1) }.is_err());
    }
}
