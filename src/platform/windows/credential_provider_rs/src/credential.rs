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
}

impl Credential {
    pub fn new() -> Self {
        Self { advised: Cell::new(false) }
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
        _pcpgsr: *mut CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE,
        _pcpcs: *mut CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION,
        _ppszoptionalstatustext: *mut PWSTR,
        _pcpsioptionalstatusicon: *mut CREDENTIAL_PROVIDER_STATUS_ICON,
    ) -> windows_core::Result<()> {
        // Phase 3: fetch the one-shot credential from the auth service,
        // serialize KERB_INTERACTIVE_UNLOCK_LOGON, mark stale.
        Err(Error::from_hresult(crate::E_NOTIMPL))
    }

    fn ReportResult(
        &self,
        _ntsstatus: NTSTATUS,
        _ntssubstatus: NTSTATUS,
        _ppszoptionalstatustext: *mut PWSTR,
        _pcpsioptionalstatusicon: *mut CREDENTIAL_PROVIDER_STATUS_ICON,
    ) -> windows_core::Result<()> {
        // Phase 3 marks the credential stale on failure so it is never
        // re-submitted with the same pipe token.
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
