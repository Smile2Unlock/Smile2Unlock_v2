//! ICredentialProviderCredential / 2 / WithFieldOptions (Phase 1).
//!
//! The v1 credential is an empty tile: it renders the status fields and the
//! submit button but holds no password material. GetSerialization stays
//! E_NOTIMPL until Phase 3 wires the auth-service pipe; ReportResult is a
//! no-op until then as well.

use core::cell::Cell;


use windows::Win32::Foundation::NTSTATUS;
use windows::Win32::Graphics::Gdi::HBITMAP;
use windows::Win32::UI::Shell::{
    CREDENTIAL_PROVIDER_CREDENTIAL_FIELD_OPTIONS,
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION,
    CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE, CREDENTIAL_PROVIDER_FIELD_STATE,
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE, CREDENTIAL_PROVIDER_STATUS_ICON,
    ICredentialProviderCredential, ICredentialProviderCredential2,
    ICredentialProviderCredential2_Impl, ICredentialProviderCredential_Impl,
    ICredentialProviderCredentialEvents,
    ICredentialProviderCredentialWithFieldOptions,
    ICredentialProviderCredentialWithFieldOptions_Impl,
};
use windows_core::{implement, Error, Ref, BOOL, PCWSTR, PWSTR};

use crate::fields::{self, FieldId};

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
}

impl Credential {
    pub fn new(scenario: i32) -> Self {
        Self {
            advised: Cell::new(false),
            scenario: Cell::new(scenario),
            stale: Cell::new(false),
            serialized: Cell::new(false),
        }
    }
}

impl ICredentialProviderCredential_Impl for Credential_Impl {
    fn Advise(&self, _pcpce: Ref<ICredentialProviderCredentialEvents>) -> windows_core::Result<()> {
        // Phase 3 stores the marshalled event pointer for stale/status pushes.
        self.advised.set(true);
        Ok(())
    }

    fn UnAdvise(&self) -> windows_core::Result<()> {
        self.advised.set(false);
        Ok(())
    }

    fn SetSelected(&self) -> windows_core::Result<BOOL> {
        // Selected tile shows FaceStatus/SubmitButton; never auto-submit.
        Ok(BOOL(1))
    }

    fn SetDeselected(&self) -> windows_core::Result<()> {
        Ok(())
    }

    fn GetFieldState(
        &self,
        dwfieldid: u32,
        pcpfs: *mut CREDENTIAL_PROVIDER_FIELD_STATE,
        pcpfis: *mut CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE,
    ) -> windows_core::Result<()> {
        let pairs = fields::state_pairs();
        let pair = match dwfieldid {
            0..=3 => &pairs[dwfieldid as usize],
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

    fn GetStringValue(&self, _dwfieldid: u32) -> windows_core::Result<PWSTR> {
        // No text-editable fields in v1.
        Err(Error::from_hresult(crate::E_NOTIMPL))
    }

    fn GetBitmapValue(&self, _dwfieldid: u32) -> windows_core::Result<HBITMAP> {
        // Tile image is drawn by LogonUI from the LOGO field guid; the
        // provider itself has no bitmap resources yet.
        Err(Error::from_hresult(crate::E_NOTIMPL))
    }

    fn GetCheckboxValue(
        &self,
        _dwfieldid: u32,
        _pbchecked: *mut BOOL,
        _ppszlabel: *mut PWSTR,
    ) -> windows_core::Result<()> {
        Err(Error::from_hresult(crate::E_NOTIMPL))
    }

    fn GetSubmitButtonValue(&self, dwfieldid: u32) -> windows_core::Result<u32> {
        if dwfieldid == FieldId::SubmitButton as u32 {
            Ok(3)
        } else {
            Err(Error::from_hresult(crate::E_INVALIDARG))
        }
    }

    fn GetComboBoxValueCount(
        &self,
        _dwfieldid: u32,
        _pcitems: *mut u32,
        _pdwselecteditem: *mut u32,
    ) -> windows_core::Result<()> {
        Err(Error::from_hresult(crate::E_NOTIMPL))
    }

    fn GetComboBoxValueAt(&self, _dwfieldid: u32, _dwitem: u32) -> windows_core::Result<PWSTR> {
        Err(Error::from_hresult(crate::E_NOTIMPL))
    }

    fn SetStringValue(&self, _dwfieldid: u32, _psz: &PCWSTR) -> windows_core::Result<()> {
        // Accept-and-ignore: v1 has no input fields, but LogonUI may probe.
        Ok(())
    }

    fn SetCheckboxValue(&self, _dwfieldid: u32, _bchecked: BOOL) -> windows_core::Result<()> {
        Ok(())
    }

    fn SetComboBoxSelectedValue(
        &self,
        _dwfieldid: u32,
        _dwselecteditem: u32,
    ) -> windows_core::Result<()> {
        Ok(())
    }

    fn CommandLinkClicked(&self, _dwfieldid: u32) -> windows_core::Result<()> {
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
            return Err(Error::from_hresult(crate::E_NOTIMPL));
        }
        let scenario = self.scenario.get();

        let sid = match crate::pipe_client::current_user_sid() {
            Ok(sid) => sid.encode_utf16().collect::<Vec<u16>>(),
            Err(_) => return Err(Error::from_hresult(crate::E_NOTIMPL)),
        };
        let password = match crate::pipe_client::PipeClient.prepare(&sid, 1, 0) {
            Ok(pw) => pw.as_u16_slice().to_vec(),
            Err(err) => return Err(err),
        };
        let protected = match crate::serialization::protect_password(&password) {
            Ok(p) => p,
            Err(err) => {
                let mut pw = password.clone();
                crate::pipe_client::secure_clear(&mut pw);
                return Err(err);
            }
        };
        let (domain, username) =
            match crate::serialization::split_domain_and_username(&protected) {
                Ok(d) => d,
                Err(err) => return Err(err),
            };
        let kiul = match crate::serialization::kerb_interactive_unlock_logon_init(
            &domain,
            &username,
            &protected,
            scenario,
        ) {
            Ok(k) => k,
            Err(err) => return Err(err),
        };
        let (blob, blob_len) = unsafe {
            match crate::serialization::kerb_interactive_unlock_logon_pack(&kiul) {
                Ok(b) => b,
                Err(err) => return Err(err),
            }
        };
        let auth_package = match crate::serialization::retrieve_negotiate_auth_package() {
            Ok(p) => p,
            Err(err) => {
                unsafe { windows::Win32::System::Com::CoTaskMemFree(Some(blob as *const _)) };
                return Err(err);
            }
        };

        let serialization = unsafe { &mut *pcpcs };
        serialization.clsidCredentialProvider = crate::CLSID_SU_PROVIDER;
        serialization.ulAuthenticationPackage = auth_package;
        serialization.rgbSerialization = blob as *mut u8;
        serialization.cbSerialization = blob_len as u32;
        if !pcpgsr.is_null() {
            unsafe { *pcpgsr = CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE(1) }; // CPGSR_RETURN_CREDENTIAL_FINISHED
        }
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
        // Phase 4 binds the pipe request to the SetUserArray SID.
        Err(Error::from_hresult(crate::E_NOTIMPL))
    }
}

impl ICredentialProviderCredentialWithFieldOptions_Impl for Credential_Impl {
    fn GetFieldOptions(
        &self,
        _fieldid: u32,
    ) -> windows_core::Result<CREDENTIAL_PROVIDER_CREDENTIAL_FIELD_OPTIONS> {
        Ok(CREDENTIAL_PROVIDER_CREDENTIAL_FIELD_OPTIONS(0))
    }
}
