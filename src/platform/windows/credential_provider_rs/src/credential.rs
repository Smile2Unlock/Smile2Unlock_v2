//! ICredentialProviderCredential.
//!
//! This build replicates the C++ baseline's full 14-field descriptor table
//! (common.h) to test whether LogonUI needs the extra hidden fields to show
//! the tile. Only the first five fields are visible; the remaining nine are
//! hidden.

use core::cell::{Cell, RefCell};

use windows::Win32::Foundation::NTSTATUS;
use windows::Win32::Graphics::Gdi::HBITMAP;
use windows::Win32::System::Com::CoTaskMemAlloc;
use windows::Win32::UI::Shell::{
    CPSI_ERROR,
    CREDENTIAL_PROVIDER_CREDENTIAL_FIELD_OPTIONS,
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION,
    CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE, CREDENTIAL_PROVIDER_FIELD_STATE,
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE, CREDENTIAL_PROVIDER_STATUS_ICON,
    ICredentialProviderCredential, ICredentialProviderCredential2,
    ICredentialProviderCredential2_Impl, ICredentialProviderCredential_Impl,
    ICredentialProviderCredentialEvents, ICredentialProviderCredentialWithFieldOptions,
    ICredentialProviderCredentialWithFieldOptions_Impl,
};
use windows_core::{implement, Error, Ref, BOOL, PCWSTR, PWSTR};

use crate::fields::{self, FieldId};
use crate::recognition::{Recognition, RS_SUCCESS};

#[implement(
    ICredentialProviderCredential,
    ICredentialProviderCredential2,
    ICredentialProviderCredentialWithFieldOptions
)]
pub struct Credential {
    /// upadvisecontext from Advise; kept so Phase 3 can push events.
    advised: Cell<bool>,
    /// usage scenario captured at creation (CPUS_LOGON/CPUS_UNLOCK_WORKSTATION).
    scenario: Cell<i32>,
    /// Set when ReportResult reports a failed login; forbids re-submission.
    stale: Cell<bool>,
    /// Set once GetSerialization returned a credential; forbids re-submission.
    serialized: Cell<bool>,
    /// SID of the user this tile is associated with (V2 CP requirement).
    user_sid: RefCell<Option<String>>,
    /// Face-recognition client shared with the provider. None when the
    /// credential is created outside a provider that owns one (never in
    /// practice); the gate then degrades to the password-only flow.
    recognition: Option<std::sync::Arc<Recognition>>,
}

impl Credential {
    pub fn new(
        scenario: i32,
        user_sid: Option<String>,
        recognition: Option<std::sync::Arc<Recognition>>,
    ) -> Self {
        crate::log::cp_log(&format!("Credential::new scenario={} sid={:?}", scenario, user_sid));
        Self {
            advised: Cell::new(false),
            scenario: Cell::new(scenario),
            stale: Cell::new(false),
            serialized: Cell::new(false),
            user_sid: RefCell::new(user_sid),
            recognition,
        }
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
        Ok(())
    }

    fn SetSelected(&self) -> windows_core::Result<BOOL> {
        crate::log::cp_log("Credential::SetSelected");
        // Do not auto-submit: the credential is only submitted after the
        // auth-service pipe has produced a password (user clicks submit or
        // Phase 3 pushes a completion event). Returning TRUE here would make
        // LogonUI call GetSerialization immediately, before the credential is
        // ready, which hides the tile on failure.
        Ok(BOOL(0))
    }

    fn SetDeselected(&self) -> windows_core::Result<()> {
        crate::log::cp_log("Credential::SetDeselected");
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
            unsafe { *pcpfis = CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE(pair.interactive as i32) };
        }
        Ok(())
    }

    fn GetStringValue(&self, dwfieldid: u32) -> windows_core::Result<PWSTR> {
        crate::log::cp_log(&format!("Credential::GetStringValue({})", dwfieldid));
        let id = Credential::field_id(dwfieldid)?;
        // LogonUI renders SMALL_TEXT/LARGE_TEXT fields from this value;
        // an error here aborts tile creation, so always hand back a string.
        let text = crate::fields::display_text(id);
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
        let row_stride = ((width as u32 * 3 + 3) / 4) * 4;
        let pixels_needed = row_stride * abs_h;
        if off_bits + pixels_needed as usize > BMP.len() {
            crate::log::cp_log("  GetBitmapValue: BMP truncated");
            return Err(Error::from_hresult(crate::E_NOTIMPL));
        }

        use windows::Win32::Graphics::Gdi::{
            CreateDIBSection, BITMAPINFO, BITMAPINFOHEADER, BI_RGB, DIB_RGB_COLORS,
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
        let hbmp = match unsafe {
            CreateDIBSection(
                None,
                &bmi,
                DIB_RGB_COLORS,
                &mut bits,
                None,
                0,
            )
        } {
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
        pbchecked: *mut BOOL,
        ppszlabel: *mut PWSTR,
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
        pcitems: *mut u32,
        pdwselecteditem: *mut u32,
    ) -> windows_core::Result<()> {
        crate::log::cp_log(&format!("Credential::GetComboBoxValueCount({})", dwfieldid));
        // The combobox field was removed from the v1 tile.
        let _ = Credential::field_id(dwfieldid)?;
        Err(Error::from_hresult(crate::E_INVALIDARG))
    }

    fn GetComboBoxValueAt(&self, dwfieldid: u32, dwitem: u32) -> windows_core::Result<PWSTR> {
        crate::log::cp_log(&format!("Credential::GetComboBoxValueAt({}, {})", dwfieldid, dwitem));
        // The combobox field was removed from the v1 tile.
        let _ = Credential::field_id(dwfieldid)?;
        Err(Error::from_hresult(crate::E_INVALIDARG))
    }

    fn SetStringValue(&self, dwfieldid: u32, _psz: &PCWSTR) -> windows_core::Result<()> {
        crate::log::cp_log(&format!("Credential::SetStringValue({})", dwfieldid));
        // Accept-and-ignore for any valid field; LogonUI may probe hidden fields.
        let _ = Credential::field_id(dwfieldid)?;
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
        crate::log::cp_log(&format!("Credential::SetComboBoxSelectedValue({})", dwfieldid));
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

        // One submission per pipe token: a failed login (ReportResult) or a
        // previous serialization must not produce a second credential.
        if self.stale.get() || self.serialized.get() {
            crate::log::cp_log(&format!(
                "GetSerialization: rejected stale={} serialized={}",
                self.stale.get(),
                self.serialized.get()
            ));
            return Err(Error::from_hresult(crate::E_NOTIMPL));
        }
        // Face-recognition gate (RecognitionMode):
        // - Manual (0, default): this GetSerialization call IS the
        //   password-box Enter trigger. Recognition runs now; success lets
        //   the stored secret through, failure rejects the submit (LogonUI
        //   shows the error status; the tile stays for a retry).
        // - Auto (1): when LogonUI auto-submits after a face success,
        //   GetCredentialCount consumed face_ready and armed auto_grant, so
        //   this gate is skipped. Otherwise (user pressed Enter before a
        //   success) it falls through to the same manual trigger.
        if let Some(rec) = self.recognition.as_ref() {
            if rec.available() {
                let granted = rec.consume_auto_grant();
                if !granted && !rec.is_ready() {
                    let outcome = rec.trigger_and_wait();
                    crate::log::cp_log(&format!(
                        "GetSerialization: face gate outcome={}",
                        outcome
                    ));
                    if outcome != RS_SUCCESS {
                        // Reject the submit with an error status; LogonUI
                        // keeps the tile and shows the text.
                        if !ppszoptionalstatustext.is_null() {
                            let msg: Vec<u16> = "Face recognition failed. Please try again."
                                .encode_utf16()
                                .chain(core::iter::once(0))
                                .collect();
                            // SAFETY: CoTaskMemAlloc returns a writable block
                            // of msg.len() u16s; LogonUI frees it with
                            // CoTaskMemFree.
                            let mem = unsafe { CoTaskMemAlloc(msg.len() * 2) } as *mut u16;
                            if !mem.is_null() {
                                unsafe {
                                    core::ptr::copy_nonoverlapping(
                                        msg.as_ptr(), mem, msg.len(),
                                    );
                                    *ppszoptionalstatustext = PWSTR(mem);
                                }
                            }
                        }
                        if !pcpsioptionalstatusicon.is_null() {
                            unsafe { *pcpsioptionalstatusicon = CPSI_ERROR };
                        }
                        if !pcpgsr.is_null() {
                            // CPGSR_NO_CREDENTIAL_FINISHED: user is done,
                            // no credential to submit.
                            unsafe {
                                *pcpgsr = CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE(1)
                            };
                        }
                        return Ok(());
                    }
                }
            }
        }

        let scenario = self.scenario.get();

        // The pipe service looks up the secret store by the REQUESTED sid
        // (the user this tile is bound to, captured from SetUserArray).
        // current_user_sid() would return LogonUI's SYSTEM SID, which has no
        // stored secret — use the tile's bound user instead.
        let sid = match self.user_sid.borrow().as_ref() {
            Some(sid) => sid.encode_utf16().collect::<Vec<u16>>(),
            None => return Err(Error::from_hresult(crate::E_NOTIMPL)),
        };
        // The service rejects repeated request_ids (replay cache). A naive
        // per-process counter starting at 1 collides with ids the service
        // has already seen (it keeps a process-lifetime cache), forcing
        // repeated submits. Seed with the current PID + timestamp so each
        // LogonUI process starts from a unique, effectively never-seen id.
        use core::sync::atomic::{AtomicU64, Ordering};
        static NEXT_REQUEST_ID: AtomicU64 = AtomicU64::new(0);
        let millis = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_millis() as u64)
            .unwrap_or(0);
        let pid = unsafe { windows::Win32::System::Threading::GetCurrentProcessId() } as u64;
        let base = (millis << 24) ^ (pid << 8) ^ (millis & 0xff);
        let request_id = if base == 0 { 1 } else { base }
            + NEXT_REQUEST_ID.fetch_add(1, Ordering::Relaxed);
        let mut password = match crate::pipe_client::PipeClient.prepare(&sid, request_id, 0) {
            Ok(pw) => {
                crate::log::cp_log("GetSerialization: pipe prepare OK");
                pw
            }
            Err(err) => {
                crate::log::cp_log(&format!(
                    "GetSerialization: pipe prepare FAILED {:08x}",
                    err.code().0
                ));
                return Err(err);
            }
        };
        // CredProtect directly over the protected-memory view; no plain
        // Vec<u16> copy of the one-time password is produced (memsafe gate).
        let mut protected = match password.with_password(|units| {
            crate::serialization::protect_password(units)
        }) {
            Ok(Ok(p)) => p,
            Ok(Err(err)) => {
                crate::log::cp_log(&format!(
                    "GetSerialization: protect_password FAILED {:08x}",
                    err.code().0
                ));
                return Err(err);
            }
            Err(_) => {
                crate::log::cp_log("GetSerialization: secret view FAILED");
                return Err(Error::from_hresult(crate::E_NOTIMPL));
            }
        };
        // Domain/username must come from the tile's bound user (resolved via
        // LookupAccountSidW), NOT from the protected password — the previous
        // split_domain_and_username(&protected) fed the encrypted blob into
        // the KERB username fields and would always fail logon.
        let sid_text = match self.user_sid.borrow().as_ref() {
            Some(sid) => sid.clone(),
            None => return Err(Error::from_hresult(crate::E_NOTIMPL)),
        };
        let (domain, username) =
            match crate::serialization::qualified_username_from_sid(&sid_text) {
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
            &domain,
            &username,
            &protected,
            scenario,
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
        serialization.rgbSerialization = blob as *mut u8;
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
        Ok(())
    }

    fn ReportResult(
        &self,
        ntsstatus: NTSTATUS,
        _ntssubstatus: NTSTATUS,
        _ppszoptionalstatustext: *mut PWSTR,
        _pcpsioptionalstatusicon: *mut CREDENTIAL_PROVIDER_STATUS_ICON,
    ) -> windows_core::Result<()> {
        crate::log::cp_log(&format!("Credential::ReportResult status={:#x}", ntsstatus.0));
        // STATUS_SUCCESS (0) means the login went through; anything else
        // marks the one-shot credential stale so the same pipe token can
        // never be re-submitted.
        if ntsstatus.0 != 0 {
            self.stale.set(true);
        }
        Ok(())
    }
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


