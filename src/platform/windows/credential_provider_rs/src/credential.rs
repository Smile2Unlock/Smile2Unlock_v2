//! ICredentialProviderCredential.
//!
//! This build replicates the C++ baseline's full 14-field descriptor table
//! (common.h) to test whether LogonUI needs the extra hidden fields to show
//! the tile. Only the first five fields are visible; the remaining nine are
//! hidden.

use core::cell::{Cell, RefCell};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

static NEXT_OWNER: AtomicU64 = AtomicU64::new(1);

use windows::Win32::Foundation::NTSTATUS;
use windows::Win32::Graphics::Gdi::HBITMAP;
use windows::Win32::System::Com::CoTaskMemAlloc;
use windows::Win32::UI::Shell::{
    CREDENTIAL_PROVIDER_CREDENTIAL_FIELD_OPTIONS, CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION,
    CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE, CREDENTIAL_PROVIDER_FIELD_STATE,
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE, CREDENTIAL_PROVIDER_STATUS_ICON,
    ICredentialProviderCredential, ICredentialProviderCredential_Impl,
    ICredentialProviderCredential2, ICredentialProviderCredential2_Impl,
    ICredentialProviderCredentialEvents, ICredentialProviderCredentialWithFieldOptions,
    ICredentialProviderCredentialWithFieldOptions_Impl,
};
use windows_core::{BOOL, Error, PCWSTR, PWSTR, Ref, implement};

use crate::auto_runtime::{AutoRuntime, PipeRecognitionTransport};
use crate::fields::{self, FieldId};

#[implement(
    ICredentialProviderCredential,
    ICredentialProviderCredential2,
    ICredentialProviderCredentialWithFieldOptions
)]
pub struct Credential {
    /// Unique even when a replaced user array contains the same SID.
    owner: u64,
    /// upadvisecontext from Advise; kept so Phase 3 can push events.
    advised: Cell<bool>,
    /// usage scenario captured at creation (CPUS_LOGON/CPUS_UNLOCK_WORKSTATION).
    scenario: Cell<i32>,
    /// Set after a successful ReportResult; prevents accidental re-submission
    /// while LogonUI tears down the credential.
    stale: Cell<bool>,
    /// Set once GetSerialization returned a credential; forbids re-submission.
    serialized: Cell<bool>,
    broker_request: Cell<Option<(u64, u32)>>,
    broker_password_invalid: Cell<bool>,
    auto_suppressed: Cell<bool>,
    /// SID of the user this tile is associated with (V2 CP requirement).
    user_sid: RefCell<Option<String>>,
    /// Password entered into the tile. A non-empty value uses the normal
    /// Windows password path and is never sent to the face-auth pipe.
    password: RefCell<Vec<u16>>,
    /// Process-wide automatic-recognition worker.
    runtime: Arc<AutoRuntime<PipeRecognitionTransport>>,
}

impl Credential {
    pub fn new(
        scenario: i32,
        user_sid: Option<String>,
        runtime: Arc<AutoRuntime<PipeRecognitionTransport>>,
    ) -> Self {
        crate::log::cp_log(&format!(
            "Credential::new scenario={} sid={:?}",
            scenario, user_sid
        ));
        Self {
            owner: NEXT_OWNER.fetch_add(1, Ordering::Relaxed),
            advised: Cell::new(false),
            scenario: Cell::new(scenario),
            stale: Cell::new(false),
            serialized: Cell::new(false),
            broker_request: Cell::new(None),
            broker_password_invalid: Cell::new(false),
            auto_suppressed: Cell::new(false),
            user_sid: RefCell::new(user_sid),
            password: RefCell::new(Vec::new()),
            runtime,
        }
    }

    /// Invalidate any in-flight automatic attempt for this tile. A late result
    /// is discarded by the runtime and never reaches LogonUI.
    fn cancel_auto(&self) {
        self.runtime.cancel_owned(self.owner);
    }

    /// Convert a raw field id to a FieldId, returning E_INVALIDARG if out of range.
    fn field_id(dwfieldid: u32) -> windows_core::Result<FieldId> {
        match dwfieldid {
            0 => Ok(FieldId::TileImage),
            1 => Ok(FieldId::Label),
            2 => Ok(FieldId::LargeText),
            3 => Ok(FieldId::PasswordText),
            4 => Ok(FieldId::SubmitButton),
            _ => Err(Error::from_hresult(crate::E_INVALIDARG)),
        }
    }

    /// Copy a Rust string into a CoTaskMem-allocated PWSTR.
    fn alloc_string(text: &str) -> windows_core::Result<PWSTR> {
        let wide: Vec<u16> = text.encode_utf16().chain(core::iter::once(0)).collect();
        let bytes = wide.len() * core::mem::size_of::<u16>();
        let mem = unsafe { CoTaskMemAlloc(bytes) };
        if mem.is_null() {
            return Err(Error::from_hresult(crate::E_OUTOFMEMORY));
        }
        unsafe {
            core::ptr::copy_nonoverlapping(wide.as_ptr(), mem as *mut u16, wide.len());
        }
        Ok(PWSTR(mem as *mut u16))
    }
}

impl Drop for Credential {
    fn drop(&mut self) {
        self.runtime.deselect_owned(self.owner);
        let mut password = self.password.borrow_mut();
        crate::pipe_client::secure_clear(&mut password);
        password.clear();
    }
}

#[cfg(test)]
impl Credential {
    /// Test constructor: a runtime whose worker is never started because the
    /// tests do not select the tile.
    pub fn new_test(scenario: i32, user_sid: Option<String>) -> Self {
        Self::new(
            scenario,
            user_sid,
            AutoRuntime::new(
                PipeRecognitionTransport,
                Box::new(crate::auto_runtime::MonotonicClock),
            ),
        )
    }

    pub(crate) fn prepare_test_grant(&self) {
        let sid = self.user_sid.borrow();
        self.runtime.publish_test_grant(
            self.owner,
            sid.as_ref().unwrap(),
            crate::pipe_client::PreparedPipePassword::test_grant(),
        );
    }
}

impl ICredentialProviderCredential_Impl for Credential_Impl {
    fn Advise(&self, _pcpce: Ref<ICredentialProviderCredentialEvents>) -> windows_core::Result<()> {
        // Phase 3 stores the marshalled event pointer for stale/status pushes.
        crate::log::cp_log("Credential::Advise");
        self.advised.set(true);
        Ok(())
    }

    fn UnAdvise(&self) -> windows_core::Result<()> {
        crate::log::cp_log("Credential::UnAdvise");
        self.advised.set(false);
        // LogonUI unadvises tiles before re-enumerating CredentialsChanged.
        // This only disconnects field events; invalidating the grant here
        // would turn the subsequent GetCredentialCount autologon flag off.
        // SetDeselected, tile destruction, user-array replacement and the
        // provider's UnAdvise own cancellation of the authentication attempt.
        Ok(())
    }

    fn SetSelected(&self) -> windows_core::Result<BOOL> {
        crate::log::cp_log("Credential::SetSelected");
        let mut autologon = false;
        if let Some(sid) = self.user_sid.borrow().as_ref().cloned() {
            let mut settings = crate::auto_runtime::read_trigger_settings(&sid);
            // Enumeration/reselection must not override a typed password or
            // retry a broker password that Windows already rejected.
            let manual_only = !self.password.borrow().is_empty()
                || self.broker_password_invalid.get()
                || self.stale.get()
                || self.serialized.get()
                || self.auto_suppressed.get();
            if manual_only {
                settings.mode = crate::auto_recognition::TriggerMode::Manual;
            }
            let generation = self.runtime.select_owned(
                self.owner,
                &sid,
                crate::pipe_client::current_session_id(),
                crate::pipe_client::next_request_id(),
                settings,
            );
            crate::log::cp_log(&format!("Credential::SetSelected generation={generation}"));
            autologon = !manual_only && self.runtime.has_ready_owned(self.owner, &sid);
        }
        // A refresh can select the tile again after CredentialsChanged. Both
        // provider enumeration and tile selection must request submission of
        // an unexpired grant; selection before recognition finishes stays FALSE.
        // Readiness is only inspected here, never consumed before serialization.
        crate::log::cp_log(&format!("Credential::SetSelected autologon={autologon}"));
        Ok(BOOL(autologon as i32))
    }

    fn SetDeselected(&self) -> windows_core::Result<()> {
        crate::log::cp_log("Credential::SetDeselected");
        self.runtime.deselect_owned(self.owner);
        self.auto_suppressed.set(false);
        Ok(())
    }

    fn GetFieldState(
        &self,
        dwfieldid: u32,
        pcpfs: *mut CREDENTIAL_PROVIDER_FIELD_STATE,
        pcpfis: *mut CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE,
    ) -> windows_core::Result<()> {
        crate::log::cp_log(&format!("Credential::GetFieldState({})", dwfieldid));
        let pairs = fields::state_pairs();
        let pair = match dwfieldid {
            0..=4 => &pairs[dwfieldid as usize],
            _ => return Err(Error::from_hresult(crate::E_INVALIDARG)),
        };
        if !pcpfs.is_null() {
            // SAFETY: caller-provided output slot.
            unsafe { *pcpfs = CREDENTIAL_PROVIDER_FIELD_STATE(pair.state as i32) };
        }
        if !pcpfis.is_null() {
            // SAFETY: caller-provided output slot.
            unsafe {
                *pcpfis = CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE(pair.interactive as i32)
            };
        }
        Ok(())
    }

    fn GetStringValue(&self, dwfieldid: u32) -> windows_core::Result<PWSTR> {
        crate::log::cp_log(&format!("Credential::GetStringValue({})", dwfieldid));
        let id = Credential::field_id(dwfieldid)?;
        // LogonUI renders SMALL_TEXT/LARGE_TEXT fields from this value;
        // an error here aborts tile creation, so always hand back a string.
        let text = if id == FieldId::LargeText {
            match self.runtime.phase_owned(self.owner) {
                Some(crate::Phase::InitialDelay) => {
                    "Face recognition will start shortly. You can enter your Windows password."
                }
                Some(crate::Phase::Recognizing | crate::Phase::RetryDelay) => {
                    "Recognizing your face. You can enter your Windows password."
                }
                Some(crate::Phase::Stopped) => {
                    "Face recognition stopped. Use your Windows password or select this option again."
                }
                _ => crate::fields::display_text(id),
            }
        } else {
            crate::fields::display_text(id)
        };
        Credential::alloc_string(text)
    }

    fn GetBitmapValue(&self, dwfieldid: u32) -> windows_core::Result<HBITMAP> {
        crate::log::cp_log(&format!("Credential::GetBitmapValue({})", dwfieldid));
        if dwfieldid != FieldId::TileImage as u32 {
            return Err(Error::from_hresult(crate::E_INVALIDARG));
        }
        // The tile image is embedded in the DLL itself (same file the C++
        // baseline links as IDB_TILE_IMAGE), so no external .bmp is needed
        // next to the DLL. Parsing the 24-bpp BMP header manually and
        // creating the HBITMAP with CreateDIBSection keeps the resource
        // inside the cdylib (Rust cdylibs cannot carry a .rc resource).
        const BMP: &[u8] = include_bytes!("../assets/tileimage.bmp");
        // BITMAPFILEHEADER: bfOffBits is a little-endian u32 at offset 10.
        // BITMAPINFOHEADER: biWidth(i32)@18, biHeight(i32)@22, biBitCount(u16)@28.
        if BMP.len() < 54 {
            crate::log::cp_log("  GetBitmapValue: BMP too short");
            return Err(Error::from_hresult(crate::E_NOTIMPL));
        }
        let off_bits = u32::from_le_bytes([BMP[10], BMP[11], BMP[12], BMP[13]]) as usize;
        let width = i32::from_le_bytes([BMP[18], BMP[19], BMP[20], BMP[21]]);
        let height = i32::from_le_bytes([BMP[22], BMP[23], BMP[24], BMP[25]]);
        let bpp = u16::from_le_bytes([BMP[28], BMP[29]]);
        if width <= 0 || height == 0 || bpp != 24 {
            crate::log::cp_log("  GetBitmapValue: bad BMP header");
            return Err(Error::from_hresult(crate::E_NOTIMPL));
        }
        let abs_h = height.unsigned_abs();
        // 24-bpp scan lines are padded to 4-byte boundaries in the file.
        let row_stride = (width as u32 * 3).div_ceil(4) * 4;
        let pixels_needed = row_stride * abs_h;
        if off_bits + pixels_needed as usize > BMP.len() {
            crate::log::cp_log("  GetBitmapValue: BMP truncated");
            return Err(Error::from_hresult(crate::E_NOTIMPL));
        }

        use windows::Win32::Graphics::Gdi::{
            BI_RGB, BITMAPINFO, BITMAPINFOHEADER, CreateDIBSection, DIB_RGB_COLORS,
        };
        let header = BITMAPINFOHEADER {
            biSize: core::mem::size_of::<BITMAPINFOHEADER>() as u32,
            biWidth: width,
            biHeight: height,
            biPlanes: 1,
            biBitCount: bpp,
            biCompression: BI_RGB.0,
            biSizeImage: pixels_needed,
            ..Default::default()
        };
        let bmi = BITMAPINFO {
            bmiHeader: header,
            bmiColors: [Default::default()],
        };
        let mut bits: *mut core::ffi::c_void = core::ptr::null_mut();
        // SAFETY: bmi is a valid initialized BITMAPINFO; LogonUI owns and
        // destroys the returned HBITMAP with DeleteObject.
        let hbmp = match unsafe { CreateDIBSection(None, &bmi, DIB_RGB_COLORS, &mut bits, None, 0) }
        {
            Ok(h) => h,
            Err(e) => {
                crate::log::cp_log(&format!(
                    "  GetBitmapValue: CreateDIBSection failed {:08x}",
                    e.code().0
                ));
                return Err(Error::from_hresult(crate::E_NOTIMPL));
            }
        };
        if bits.is_null() {
            crate::log::cp_log("  GetBitmapValue: CreateDIBSection null bits");
            return Err(Error::from_hresult(crate::E_NOTIMPL));
        }
        // Positive height means bottom-up rows, which matches the file layout;
        // copy row by row. No NUL-termination or endianness conversion needed
        // for RGB triples.
        // SAFETY: bits points to pixels_needed writable bytes (DIBSection).
        unsafe {
            core::ptr::copy_nonoverlapping(
                BMP.as_ptr().add(off_bits),
                bits as *mut u8,
                pixels_needed as usize,
            );
        }
        crate::log::cp_log(&format!(
            "  GetBitmapValue: OK {}x{} bpp={}",
            width, abs_h, bpp
        ));
        Ok(hbmp)
    }

    fn GetCheckboxValue(
        &self,
        dwfieldid: u32,
        _pbchecked: *mut BOOL,
        _ppszlabel: *mut PWSTR,
    ) -> windows_core::Result<()> {
        crate::log::cp_log(&format!("Credential::GetCheckboxValue({})", dwfieldid));
        // The checkbox field was removed from the v1 tile; keep the method
        // for interface completeness but reject any probe.
        let _ = Credential::field_id(dwfieldid)?;
        Err(Error::from_hresult(crate::E_INVALIDARG))
    }

    fn GetSubmitButtonValue(&self, dwfieldid: u32) -> windows_core::Result<u32> {
        crate::log::cp_log(&format!("Credential::GetSubmitButtonValue({})", dwfieldid));
        if dwfieldid == FieldId::SubmitButton as u32 {
            // Place the submit button adjacent to the password field. LogonUI
            // requires this reference to be an input field, not the button
            // itself or a bitmap.
            Ok(FieldId::PasswordText as u32)
        } else {
            Err(Error::from_hresult(crate::E_INVALIDARG))
        }
    }

    fn GetComboBoxValueCount(
        &self,
        dwfieldid: u32,
        _pcitems: *mut u32,
        _pdwselecteditem: *mut u32,
    ) -> windows_core::Result<()> {
        crate::log::cp_log(&format!("Credential::GetComboBoxValueCount({})", dwfieldid));
        // The combobox field was removed from the v1 tile.
        let _ = Credential::field_id(dwfieldid)?;
        Err(Error::from_hresult(crate::E_INVALIDARG))
    }

    fn GetComboBoxValueAt(&self, dwfieldid: u32, dwitem: u32) -> windows_core::Result<PWSTR> {
        crate::log::cp_log(&format!(
            "Credential::GetComboBoxValueAt({}, {})",
            dwfieldid, dwitem
        ));
        // The combobox field was removed from the v1 tile.
        let _ = Credential::field_id(dwfieldid)?;
        Err(Error::from_hresult(crate::E_INVALIDARG))
    }

    fn SetStringValue(&self, dwfieldid: u32, psz: &PCWSTR) -> windows_core::Result<()> {
        crate::log::cp_log(&format!("Credential::SetStringValue({})", dwfieldid));
        let field = Credential::field_id(dwfieldid)?;
        if field == FieldId::PasswordText {
            let mut length = 0usize;
            while !psz.0.is_null() && length < 512 && unsafe { *psz.0.add(length) } != 0 {
                length += 1;
            }
            if length == 512 {
                return Err(Error::from_hresult(crate::E_INVALIDARG));
            }
            let mut password = self.password.borrow_mut();
            // LogonUI can initialize/refresh an already empty edit field.
            // This is not password typing and must not erase a pending grant.
            if length == 0 && password.is_empty() {
                return Ok(());
            }
            // A real edit, including clearing a previously typed password,
            // overrides recognition and invalidates any prepared credential.
            self.cancel_auto();
            crate::pipe_client::secure_clear(&mut password);
            password.clear();
            if length != 0 {
                let value = unsafe { core::slice::from_raw_parts(psz.0, length) };
                password.extend_from_slice(value);
            }
        }
        Ok(())
    }

    fn SetCheckboxValue(&self, dwfieldid: u32, _bchecked: BOOL) -> windows_core::Result<()> {
        crate::log::cp_log(&format!("Credential::SetCheckboxValue({})", dwfieldid));
        Ok(())
    }

    fn SetComboBoxSelectedValue(
        &self,
        dwfieldid: u32,
        _dwselecteditem: u32,
    ) -> windows_core::Result<()> {
        crate::log::cp_log(&format!(
            "Credential::SetComboBoxSelectedValue({})",
            dwfieldid
        ));
        Ok(())
    }

    fn CommandLinkClicked(&self, dwfieldid: u32) -> windows_core::Result<()> {
        crate::log::cp_log(&format!("Credential::CommandLinkClicked({})", dwfieldid));
        // No command links in v1.
        Ok(())
    }

    fn GetSerialization(
        &self,
        pcpgsr: *mut CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE,
        pcpcs: *mut CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION,
        ppszoptionalstatustext: *mut PWSTR,
        pcpsioptionalstatusicon: *mut CREDENTIAL_PROVIDER_STATUS_ICON,
    ) -> windows_core::Result<()> {
        crate::log::cp_log("Credential::GetSerialization enter");
        // Output contract (matches the C++ baseline): default to "not
        // finished", then fill in on success.
        if !pcpgsr.is_null() {
            unsafe { *pcpgsr = CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE(0) }; // CPGSR_NO_CREDENTIAL_NOT_FINISHED
        }
        if !ppszoptionalstatustext.is_null() {
            unsafe { *ppszoptionalstatustext = PWSTR::null() };
        }
        if !pcpsioptionalstatusicon.is_null() {
            unsafe { *pcpsioptionalstatusicon = CREDENTIAL_PROVIDER_STATUS_ICON(0) }; // CPSI_NONE
        }
        if pcpcs.is_null() || pcpgsr.is_null() {
            return Err(Error::from_hresult(crate::E_POINTER));
        }

        // Only one serialization may be outstanding. ReportResult resets the
        // state after a failed Windows logon so the user can retry.
        if self.stale.get() || self.serialized.get() {
            crate::log::cp_log(&format!(
                "GetSerialization: rejected stale={} serialized={}",
                self.stale.get(),
                self.serialized.get()
            ));
            return Err(Error::from_hresult(crate::E_NOTIMPL));
        }
        let has_manual_password = !self.password.borrow().is_empty();
        if !has_manual_password && self.broker_password_invalid.get() {
            return Err(crate::pipe_client::win32_error(1323));
        }

        unsafe { *pcpcs = CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION::default() };
        let scenario = self.scenario.get();

        // The pipe service looks up the secret store by the REQUESTED sid
        // (the user this tile is bound to, captured from SetUserArray).
        // current_user_sid() would return LogonUI's SYSTEM SID, which has no
        // stored secret — use the tile's bound user instead.
        let sid_text = match self.user_sid.borrow().as_ref() {
            Some(sid) => sid.clone(),
            None => return Err(Error::from_hresult(crate::E_NOTIMPL)),
        };
        let mut broker_request = None;
        let mut protected = if has_manual_password {
            self.cancel_auto();
            let mut entered = self.password.borrow_mut();
            let result = crate::serialization::protect_password(&entered);
            crate::pipe_client::secure_clear(&mut entered);
            entered.clear();
            result?
        } else if let Some((mut grant, request_id, session_id)) =
            self.runtime.take_ready_owned(self.owner, &sid_text)
        {
            // Automatic recognition already authenticated this face and the
            // service released the one-time secret. Consume it exactly once;
            // no second camera call is made for a grant the user just earned.
            crate::log::cp_log("GetSerialization: consuming automatic face grant");
            broker_request = Some((request_id, session_id));
            match grant.with_password(crate::serialization::protect_password) {
                Ok(Ok(p)) => p,
                Ok(Err(err)) => return Err(err),
                Err(_) => return Err(Error::from_hresult(crate::E_NOTIMPL)),
            }
        } else {
            let pending = self.runtime.submit_owned(
                self.owner,
                &sid_text,
                crate::pipe_client::current_session_id(),
                crate::pipe_client::next_request_id(),
            );
            if !ppszoptionalstatustext.is_null() {
                unsafe {
                    *ppszoptionalstatustext = Credential::alloc_string(if pending {
                        "Recognizing your face. You can enter your Windows password."
                    } else {
                        "Face recognition is not ready. Use your Windows password or select this option again."
                    })?
                };
            }
            // CPGSR_NO_CREDENTIAL_NOT_FINISHED: the same worker will notify
            // LogonUI and publish an autologon tile when the result is ready.
            return Ok(());
        };
        let (domain, username) = match crate::serialization::qualified_username_from_sid(&sid_text)
        {
            Ok(d) => d,
            Err(err) => {
                crate::log::cp_log("GetSerialization: username resolve failed");
                return Err(err);
            }
        };
        // Manual KERB packing, mirroring the C++ baseline exactly
        // (KerbInteractiveUnlockLogonInit + Pack). The C++ build was
        // verified to log on with this layout; CredPackAuthenticationBuffer
        // output differs and was not accepted by LSA in testing.
        let kiul = match crate::serialization::kerb_interactive_unlock_logon_init(
            &domain, &username, &protected, scenario,
        ) {
            Ok(k) => k,
            Err(err) => {
                crate::log::cp_log("GetSerialization: kerb init FAILED");
                crate::pipe_client::secure_clear(&mut protected);
                return Err(err);
            }
        };
        let (blob, blob_len) = unsafe {
            match crate::serialization::kerb_interactive_unlock_logon_pack(&kiul) {
                Ok(b) => b,
                Err(err) => {
                    crate::log::cp_log("GetSerialization: kerb pack FAILED");
                    crate::pipe_client::secure_clear(&mut protected);
                    return Err(err);
                }
            }
        };
        let auth_package = match crate::serialization::retrieve_negotiate_auth_package() {
            Ok(p) => p,
            Err(err) => {
                crate::log::cp_log("GetSerialization: negotiate package FAILED");
                unsafe { windows::Win32::System::Com::CoTaskMemFree(Some(blob as *const _)) };
                crate::pipe_client::secure_clear(&mut protected);
                return Err(err);
            }
        };

        let serialization = unsafe { &mut *pcpcs };
        serialization.clsidCredentialProvider = crate::CLSID_SU_PROVIDER;
        serialization.ulAuthenticationPackage = auth_package;
        serialization.rgbSerialization = blob;
        serialization.cbSerialization = blob_len as u32;
        if !pcpgsr.is_null() {
            unsafe { *pcpgsr = CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE(2) }; // CPGSR_RETURN_CREDENTIAL_FINISHED
        }
        crate::log::cp_log(&format!(
            "GetSerialization: done domain={} user={} cb={}",
            String::from_utf16_lossy(&domain),
            String::from_utf16_lossy(&username),
            blob_len
        ));
        crate::pipe_client::secure_clear(&mut protected);
        self.serialized.set(true);
        self.broker_request.set(broker_request);
        Ok(())
    }

    fn ReportResult(
        &self,
        ntsstatus: NTSTATUS,
        ntssubstatus: NTSTATUS,
        ppszoptionalstatustext: *mut PWSTR,
        pcpsioptionalstatusicon: *mut CREDENTIAL_PROVIDER_STATUS_ICON,
    ) -> windows_core::Result<()> {
        crate::log::cp_log(&format!(
            "Credential::ReportResult status={:#x}",
            ntsstatus.0
        ));
        // Each brokered retry gets a fresh request id and a fresh face check.
        // A failed Windows logon must therefore return the tile to its input
        // state instead of permanently disabling it.
        self.serialized.set(false);
        self.stale.set(ntsstatus.0 == 0);
        self.auto_suppressed.set(ntsstatus.0 < 0);
        // A failed Windows logon stops automatic retries: repeatedly submitting
        // the same stale secret would lock the account. The user can reselect
        // the tile to start a fresh attempt.
        self.cancel_auto();
        if !ppszoptionalstatustext.is_null() {
            unsafe { *ppszoptionalstatustext = PWSTR::null() };
        }
        if !pcpsioptionalstatusicon.is_null() {
            unsafe { *pcpsioptionalstatusicon = CREDENTIAL_PROVIDER_STATUS_ICON(0) };
        }
        if let Some((request_id, session_id)) = self.broker_request.take()
            && password_requires_refresh(ntsstatus.0, ntssubstatus.0)
        {
            // A failed broker notification must not permit repeated old-password
            // submissions from this tile. Manual Windows passwords remain usable.
            self.broker_password_invalid.set(true);
            if let Some(sid) = self.user_sid.borrow().as_ref() {
                let sid = sid.encode_utf16().collect::<Vec<_>>();
                if let Err(error) =
                    crate::pipe_client::PipeClient.mark_stale(&sid, request_id, session_id)
                {
                    crate::log::cp_log(&format!("mark_stale failed: {:08x}", error.code().0));
                }
            }
            if !ppszoptionalstatustext.is_null() {
                unsafe {
                    *ppszoptionalstatustext = Credential::alloc_string(
                        "Update the saved Windows password in Smile2Unlock. Use your Windows password to sign in.",
                    )?;
                }
            }
            if !pcpsioptionalstatusicon.is_null() {
                unsafe { *pcpsioptionalstatusicon = CREDENTIAL_PROVIDER_STATUS_ICON(1) };
            }
        }
        Ok(())
    }
}

fn password_requires_refresh(status: i32, substatus: i32) -> bool {
    if status >= 0 {
        return false;
    }
    let reason = if substatus < 0 { substatus } else { status } as u32;
    matches!(reason, 0xc000006a | 0xc000006d | 0xc0000071 | 0xc0000224)
}

impl ICredentialProviderCredential2_Impl for Credential_Impl {
    fn GetUserSid(&self) -> windows_core::Result<PWSTR> {
        crate::log::cp_log("Credential::GetUserSid");
        // V2 rule: a provider implementing ICredentialProviderSetUserArray
        // must return a SID matching a user in the array via
        // ICredentialProviderCredential2::GetUserSid, or LogonUI discards
        // the tile without showing it. The SID was captured in
        // Provider::SetUserArray (first user), mirroring the C++ baseline.
        match self.user_sid.borrow().as_ref() {
            Some(sid) => Credential::alloc_string(sid),
            // Mirrors the C++ baseline: S_FALSE + null SID associates the
            // credential with an empty user tile instead of discarding it.
            None => Err(Error::from_hresult(crate::S_FALSE)),
        }
    }
}

impl ICredentialProviderCredentialWithFieldOptions_Impl for Credential_Impl {
    fn GetFieldOptions(
        &self,
        fieldid: u32,
    ) -> windows_core::Result<CREDENTIAL_PROVIDER_CREDENTIAL_FIELD_OPTIONS> {
        crate::log::cp_log(&format!("Credential::GetFieldOptions({})", fieldid));
        let id = Credential::field_id(fieldid)?;
        // Official wincred.h values for CREDENTIAL_PROVIDER_CREDENTIAL_
        // FIELD_OPTIONS (winsdk-10 10.0.16299.0): CPCFO_NONE=0,
        // CPCFO_ENABLE_PASSWORD_REVEAL=0x1, CPCFO_IS_EMAIL_ADDRESS=0x2,
        // CPCFO_ENABLE_TOUCH_KEYBOARD_AUTO_INVOKE=0x4, CPCFO_NUMBERS_ONLY=0x8,
        // CPCFO_SHOW_ENGLISH_KEYBOARD=0x10. Mirrors the C++ baseline
        // CSampleCredential::GetFieldOptions: password reveal on the password
        // field, touch-keyboard auto-invoke on the tile image.
        const CPCFO_NONE: i32 = 0;
        const CPCFO_ENABLE_PASSWORD_REVEAL: i32 = 1;
        const CPCFO_ENABLE_TOUCH_KEYBOARD_AUTO_INVOKE: i32 = 4;
        let options = match id {
            FieldId::PasswordText => CPCFO_ENABLE_PASSWORD_REVEAL,
            FieldId::TileImage => CPCFO_ENABLE_TOUCH_KEYBOARD_AUTO_INVOKE,
            _ => CPCFO_NONE,
        };
        Ok(CREDENTIAL_PROVIDER_CREDENTIAL_FIELD_OPTIONS(options))
    }
}
#[cfg(test)]
mod password_result_tests {
    use super::*;

    #[test]
    fn empty_password_refresh_preserves_recognition_but_real_edits_cancel() {
        let object = windows_core::ComObject::new(Credential::new_test(
            1,
            Some("S-1-5-21-198101-198102-198103-1001".to_owned()),
        ));
        let credential = object.to_interface::<ICredentialProviderCredential>();
        let empty = [0u16];
        // An empty edit-field initialization must not cancel a pending worker.
        object.runtime.select_owned(
            object.owner,
            object.user_sid.borrow().as_ref().unwrap(),
            1,
            7,
            crate::TriggerSettings::from_values(Some(1), Some(60), Some(1), Some(600)),
        );
        let generation = object.runtime.generation();
        unsafe { credential.SetStringValue(3, PCWSTR(empty.as_ptr())) }.unwrap();
        unsafe { credential.SetStringValue(3, PCWSTR::null()) }.unwrap();
        assert_eq!(object.runtime.generation(), generation);
        assert_eq!(object.runtime.phase(), crate::Phase::InitialDelay);
        object.runtime.cancel();

        object.prepare_test_grant();
        unsafe { credential.SetStringValue(3, PCWSTR(empty.as_ptr())) }.unwrap();
        unsafe { credential.SetStringValue(3, PCWSTR::null()) }.unwrap();
        assert_eq!(object.runtime.phase(), crate::Phase::Ready);
        let typed = [b'x' as u16, 0];
        unsafe { credential.SetStringValue(3, PCWSTR(typed.as_ptr())) }.unwrap();
        assert!(object.runtime.ready_sid().is_none());
        assert_eq!(object.runtime.phase(), crate::Phase::Idle);
        // Clearing a previously typed password is a real edit too.
        object.prepare_test_grant();
        unsafe { credential.SetStringValue(3, PCWSTR::null()) }.unwrap();
        assert!(object.runtime.ready_sid().is_none());
        assert!(object.password.borrow().is_empty());
        object.runtime.shutdown();
    }

    #[test]
    fn invalid_or_expired_broker_password_requires_refresh() {
        for status in [0xc000006au32, 0xc000006d, 0xc0000071, 0xc0000224] {
            assert!(password_requires_refresh(status as i32, 0));
        }
        assert!(password_requires_refresh(
            0xc000006e_u32 as i32,
            0xc0000071_u32 as i32
        ));
    }

    #[test]
    fn success_and_account_restrictions_do_not_invalidate_password() {
        assert!(!password_requires_refresh(0, 0));
        assert!(!password_requires_refresh(0, 0xc000006a_u32 as i32));
        for status in [0xc0000234u32, 0xc0000072, 0xc000015b] {
            assert!(!password_requires_refresh(status as i32, 0));
            assert!(!password_requires_refresh(
                0xc000006d_u32 as i32,
                status as i32
            ));
        }
    }

    #[test]
    fn manual_password_failure_keeps_broker_available() {
        let object = windows_core::ComObject::new(Credential::new_test(1, None));
        object.serialized.set(true);
        let credential = object.to_interface::<ICredentialProviderCredential>();
        let mut text = PWSTR::null();
        let mut icon = CREDENTIAL_PROVIDER_STATUS_ICON(1);
        unsafe {
            credential
                .ReportResult(
                    NTSTATUS(0xc000006d_u32 as i32),
                    NTSTATUS(0xc000006a_u32 as i32),
                    &mut text,
                    &mut icon,
                )
                .unwrap();
        }
        assert!(!object.serialized.get());
        assert!(!object.broker_password_invalid.get());
        assert!(text.is_null());
        assert_eq!(icon.0, 0);
    }

    #[test]
    fn invalid_broker_password_blocks_resubmission_even_without_a_service() {
        let object = windows_core::ComObject::new(Credential::new_test(1, None));
        object.broker_request.set(Some((1, 1)));
        object.serialized.set(true);
        let credential = object.to_interface::<ICredentialProviderCredential>();
        unsafe {
            credential
                .ReportResult(
                    NTSTATUS(0xc000006d_u32 as i32),
                    NTSTATUS(0xc000006a_u32 as i32),
                    core::ptr::null_mut(),
                    core::ptr::null_mut(),
                )
                .unwrap();
        }
        assert!(object.broker_password_invalid.get());
        assert!(object.broker_request.get().is_none());
        let mut response = CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE(0);
        let mut serialization = CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION::default();
        let error = unsafe {
            credential
                .GetSerialization(
                    &mut response,
                    &mut serialization,
                    core::ptr::null_mut(),
                    core::ptr::null_mut(),
                )
                .unwrap_err()
        };
        assert_eq!(error.code(), crate::pipe_client::win32_error(1323).code());
    }

    #[test]
    fn early_submit_returns_pending_without_a_synchronous_pipe_call() {
        let object =
            windows_core::ComObject::new(Credential::new_test(1, Some("S-1-5-21-1".to_owned())));
        let generation = object.runtime.select_owned(
            object.owner,
            "S-1-5-21-1",
            1,
            7,
            crate::TriggerSettings::from_values(Some(1), Some(60), Some(1), Some(600)),
        );
        let credential = object.to_interface::<ICredentialProviderCredential>();
        let mut response = CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE(99);
        let mut serialization = CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION::default();
        let started = std::time::Instant::now();
        unsafe {
            credential.GetSerialization(
                &mut response,
                &mut serialization,
                core::ptr::null_mut(),
                core::ptr::null_mut(),
            )
        }
        .unwrap();
        assert!(started.elapsed() < std::time::Duration::from_millis(200));
        assert_eq!(response.0, 0);
        assert!(serialization.rgbSerialization.is_null());
        assert!(!object.serialized.get());
        assert_eq!(object.runtime.generation(), generation);
        assert_eq!(object.runtime.phase(), crate::Phase::InitialDelay);
        object.runtime.shutdown();
    }
}
