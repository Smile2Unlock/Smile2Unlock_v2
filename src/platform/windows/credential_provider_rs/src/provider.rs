//! ICredentialProvider + ICredentialProviderSetUserArray (Phase 1).
//!
//! The provider exposes the v1 field set (see fields.rs) and hands out a
//! single Credential tile. It accepts CPUS_LOGON and CPUS_UNLOCK_WORKSTATION
//! only; all other scenarios (including CPUS_CREDUI) are E_NOTIMPL so
//! LogonUI never shows this provider in credential-ui flows.
//!
//! Field descriptors are allocated with CoTaskMemAlloc because LogonUI
//! releases them with CoTaskMemFree.

use core::cell::{Cell, RefCell};
use core::ptr;

use windows::Win32::UI::Shell::{
    CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR, CREDENTIAL_PROVIDER_FIELD_TYPE,
    CREDENTIAL_PROVIDER_USAGE_SCENARIO, CPUS_LOGON, CPUS_UNLOCK_WORKSTATION,
    ICredentialProvider, ICredentialProvider_Impl,
    ICredentialProviderCredential, ICredentialProviderEvents,
    ICredentialProviderSetUserArray, ICredentialProviderSetUserArray_Impl,
    ICredentialProviderUserArray,
};
use windows::Win32::System::Com::{
    CoCreateInstance, CoTaskMemAlloc, CoTaskMemFree, CLSCTX_INPROC_SERVER,
    IGlobalInterfaceTable,
};
use windows_core::{implement, Error, GUID, Interface, Ref, BOOL, PWSTR};

/// CLSID_StdGlobalInterfaceTable {00000323-0000-0000-C000-000000000046}
/// (not exposed by windows 0.62.2 metadata).
const CLSID_STD_GLOBAL_INTERFACE_TABLE: GUID = GUID {
    data1: 0x0000_0323,
    data2: 0x0000,
    data3: 0x0000,
    data4: [0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46],
};

/// IGlobalInterfaceTable is !Send/!Sync in the windows crate (it wraps a raw
/// COM pointer), but GIT is explicitly designed for cross-thread marshalling:
/// GetInterfaceFromGlobal/RevokeInterfaceFromGlobal are documented thread-
/// safe. The wrapper restores Send/Sync for the recognition worker callback.
///
/// NOTE: the closure must call a METHOD on the wrapper, not touch its field:
/// edition-2021 disjoint closure captures would otherwise capture only the
/// inner `IGlobalInterfaceTable` (losing the Send/Sync impl).
struct GitSend(IGlobalInterfaceTable);
unsafe impl Send for GitSend {}
unsafe impl Sync for GitSend {}

impl GitSend {
    /// Retrieve the marshalled ICredentialProviderEvents proxy for the
    /// cookie registered at Advise time.
    fn get_events(&self, cookie: u32) -> windows_core::Result<ICredentialProviderEvents> {
        let mut raw: *mut core::ffi::c_void = core::ptr::null_mut();
        unsafe {
            self.0
                .GetInterfaceFromGlobal(cookie, &ICredentialProviderEvents::IID, &mut raw)
        }?;
        if raw.is_null() {
            return Err(Error::from_hresult(crate::E_POINTER));
        }
        // SAFETY: raw came from GIT for the requested IID; the proxy is
        // thread-local to this worker thread.
        Ok(unsafe { ICredentialProviderEvents::from_raw(raw) })
    }
}

use crate::credential::Credential;
use crate::fields::{self, FieldId};
use crate::recognition::{load_config, Recognition};

#[implement(ICredentialProvider, ICredentialProviderSetUserArray)]
pub struct Provider {
    /// CPUS_* value accepted by SetUsageScenario; None until set.
    usage_scenario: Cell<Option<i32>>,
    /// upadvisecontext from the last Advise call (Phase 2 stores the
    /// marshalled ICredentialProviderEvents pointer itself).
    advised_context: Cell<usize>,
    /// SID of the first user passed via SetUserArray. V2 credential providers
    /// must return a valid SID from ICredentialProviderCredential2::GetUserSid
    /// or LogonUI discards the tile.
    user_sid: RefCell<Option<String>>,
    /// GIT used to marshal ICredentialProviderEvents for the recognition
    /// worker thread (CredentialsChanged must not cross apartments raw).
    git: RefCell<Option<IGlobalInterfaceTable>>,
    git_cookie: Cell<Option<u32>>,
    /// Face-recognition client (UDP) with trigger workers. Shared with the
    /// credential so GetSerialization can gate on face success and arm the
    /// manual trigger.
    recognition: RefCell<Option<std::sync::Arc<Recognition>>>,
}

impl Provider {
    pub fn new() -> Self {
        Self {
            usage_scenario: Cell::new(None),
            advised_context: Cell::new(0),
            user_sid: RefCell::new(None),
            git: RefCell::new(None),
            git_cookie: Cell::new(None),
            recognition: RefCell::new(None),
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
        let field_type =
            CREDENTIAL_PROVIDER_FIELD_TYPE(fields::field_type(id) as i32);
        let (d1, d2, d3, d4) = fields::field_type_guid(id);
        let guid = GUID { data1: d1, data2: d2, data3: d3, data4: d4 };
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

        // Marshal ICredentialProviderEvents through the Global Interface
        // Table so the recognition worker thread can safely call
        // CredentialsChanged (no raw cross-thread COM pointer).
        let git: IGlobalInterfaceTable = unsafe {
            CoCreateInstance(&CLSID_STD_GLOBAL_INTERFACE_TABLE, None, CLSCTX_INPROC_SERVER)
        }?;
        let Some(events) = pcpe.as_ref() else {
            crate::log::cp_log("Provider::Advise: no events -> no auto-submit");
            return Ok(());
        };
        let cookie = unsafe { git.RegisterInterfaceInGlobal(events, &ICredentialProviderEvents::IID) }?;
        *self.git.borrow_mut() = Some(git);
        self.git_cookie.set(Some(cookie));

        // Face recognition: load config, create the client, arm the worker
        // for the configured mode, and wire the success callback that pushes
        // CredentialsChanged (LogonUI re-enumerates and auto-submits).
        let cfg = load_config();
        let recognition = Recognition::new(cfg);
        if recognition.available() {
            let git_clone = self.git.borrow().clone();
            let cookie_val = self.git_cookie.get();
            let ctx = upadvisecontext;
            let git_send = GitSend(git_clone.expect("git registered above"));
            let cookie = cookie_val.expect("cookie set above");
            recognition.set_on_success(Box::new(move || {
                crate::log::cp_log("Provider::on_recognition_success");
                // SAFETY: worker thread owns a marshalled copy; the GIT call
                // returns a thread-local interface proxy.
                let events = match git_send.get_events(cookie) {
                    Ok(e) => e,
                    Err(e) => {
                        crate::log::cp_log(&format!(
                            "recognition success: GetInterfaceFromGlobal FAILED {:08x}",
                            e.code().0
                        ));
                        return;
                    }
                };
                // SAFETY: events is a valid proxy; CredentialsChanged is a
                // plain method call on the worker thread.
                if let Err(e) = unsafe { events.CredentialsChanged(ctx) } {
                    crate::log::cp_log(&format!(
                        "recognition success: CredentialsChanged FAILED {:08x}",
                        e.code().0
                    ));
                }
            }));
            match cfg.mode {
                1 => recognition.start_auto(),
                _ => recognition.start_manual(),
            }
            *self.recognition.borrow_mut() = Some(std::sync::Arc::new(recognition));
        } else {
            crate::log::cp_log("Provider::Advise: recognition unavailable (degraded)");
        }
        Ok(())
    }

    fn UnAdvise(&self) -> windows_core::Result<()> {
        crate::log::cp_log("Provider::UnAdvise");
        if let Some(rec) = self.recognition.borrow_mut().take() {
            rec.stop();
        }
        if let Some(git) = self.git.borrow_mut().take() {
            if let Some(cookie) = self.git_cookie.take() {
                // SAFETY: cookie was returned by RegisterInterfaceInGlobal.
                let _ = unsafe { git.RevokeInterfaceFromGlobal(cookie) };
            }
        }
        self.advised_context.set(0);
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
        let desc_mem = unsafe { CoTaskMemAlloc(desc_bytes) } as *mut CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR;
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
        // One tile. Face recognition success arms auto-logon exactly once
        // (LogonUI immediately calls GetSerialization on the credential).
        let mut autologon = false;
        if let Some(rec) = self.recognition.borrow().as_ref() {
            if rec.consume_ready() {
                autologon = true;
                // LogonUI calls GetSerialization right after this
                // auto-logon enumeration; arm the grant so that call skips
                // the manual face gate.
                rec.mark_auto_grant();
            }
        }
        if !pdwcount.is_null() {
            unsafe { *pdwcount = 1 };
        }
        if !pdwdefault.is_null() {
            unsafe { *pdwdefault = 0 };
        }
        if !pbautologonwithdefault.is_null() {
            unsafe { *pbautologonwithdefault = BOOL(autologon as i32) };
        }
        crate::log::cp_log(&format!(
            "Provider::GetCredentialCount autologon={}",
            autologon
        ));
        Ok(())
    }

    fn GetCredentialAt(&self, dwindex: u32) -> windows_core::Result<ICredentialProviderCredential> {
        crate::log::cp_log(&format!("Provider::GetCredentialAt({})", dwindex));
        if dwindex != 0 {
            return Err(Error::from_hresult(crate::E_INVALIDARG));
        }
        let credential: ICredentialProviderCredential = Credential::new(
            self.usage_scenario.get().unwrap_or(0),
            self.user_sid.borrow().clone(),
            self.recognition.borrow().clone(),
        )
        .into();

        // Diagnostic self-test: exercise the credential immediately to prove
        // it is functional. If LogonUI never calls credential methods, we want
        // to know whether the object itself is broken.
        crate::log::cp_log("Provider::GetCredentialAt: self-test start");
        match unsafe { credential.SetSelected() } {
            Ok(b) => crate::log::cp_log(&format!("  SetSelected -> {}", b.0)),
            Err(e) => crate::log::cp_log(&format!("  SetSelected FAILED: {:08x}", e.code().0)),
        }
        match unsafe { credential.GetStringValue(2) } {
            Ok(s) => {
                let txt = unsafe { s.to_string().unwrap_or_default() };
                crate::log::cp_log(&format!("  GetStringValue(2) -> '{}'", txt));
            }
            Err(e) => crate::log::cp_log(&format!("  GetStringValue(2) FAILED: {:08x}", e.code().0)),
        }
        match credential.cast::<windows_core::IUnknown>() {
            Ok(_) => crate::log::cp_log("  cast IUnknown -> ok"),
            Err(e) => crate::log::cp_log(&format!("  cast IUnknown FAILED: {:08x}", e.code().0)),
        }
        // Real-QI checks for the V2/WFO surfaces inside the actual LogonUI
        // process, mirroring what enum_probe does out-of-process.
        match credential.cast::<windows::Win32::UI::Shell::ICredentialProviderCredential2>() {
            Ok(c2) => {
                crate::log::cp_log("  cast Credential2 -> ok");
                match unsafe { c2.GetUserSid() } {
                    Ok(sid) => {
                        if sid.0.is_null() {
                            crate::log::cp_log("  GetUserSid -> <null> (S_FALSE empty tile)");
                        } else {
                            let txt = unsafe { sid.to_string().unwrap_or_default() };
                            crate::log::cp_log(&format!("  GetUserSid -> '{}'", txt));
                            // SAFETY: SID was CoTaskMemAlloc'd by our own code.
                            unsafe {
                                windows::Win32::System::Com::CoTaskMemFree(Some(sid.0 as *const _))
                            };
                        }
                    }
                    Err(e) => crate::log::cp_log(&format!("  GetUserSid FAILED: {:08x}", e.code().0)),
                }
            }
            Err(e) => crate::log::cp_log(&format!("  cast Credential2 FAILED: {:08x}", e.code().0)),
        }
        match credential.cast::<windows::Win32::UI::Shell::ICredentialProviderCredentialWithFieldOptions>()
        {
            Ok(wfo) => {
                crate::log::cp_log("  cast WithFieldOptions -> ok");
                match unsafe { wfo.GetFieldOptions(3) } {
                    Ok(o) => crate::log::cp_log(&format!("  GetFieldOptions(3) -> {}", o.0)),
                    Err(e) => {
                        crate::log::cp_log(&format!("  GetFieldOptions FAILED: {:08x}", e.code().0))
                    }
                }
            }
            Err(e) => crate::log::cp_log(&format!("  cast WithFieldOptions FAILED: {:08x}", e.code().0)),
        }
        crate::log::cp_log("Provider::GetCredentialAt: self-test end");

        Ok(credential)
    }
}

impl ICredentialProviderSetUserArray_Impl for Provider_Impl {
    fn SetUserArray(&self, users: Ref<ICredentialProviderUserArray>) -> windows_core::Result<()> {
        crate::log::cp_log("Provider::SetUserArray");
        // V2 credential providers must associate the tile with a valid user
        // SID. Capture the first enumerated user; if the array is empty the
        // credential will return S_FALSE + NULL from GetUserSid and may be
        // shown as an empty user tile.
        if let Some(array) = users.as_ref() {
            if let Ok(count) = unsafe { array.GetCount() } {
                crate::log::cp_log(&format!("Provider::SetUserArray count={}", count));
                if count > 0 {
                    if let Ok(user) = unsafe { array.GetAt(0) } {
                        if let Ok(sid) = unsafe { user.GetSid() } {
                            if let Ok(text) = unsafe { sid.to_string() } {
                                crate::log::cp_log(&format!("Provider::SetUserArray sid={}", text));
                                *self.user_sid.borrow_mut() = Some(text);
                            }
                        }
                    }
                }
            }
        }
        Ok(())
    }
}
