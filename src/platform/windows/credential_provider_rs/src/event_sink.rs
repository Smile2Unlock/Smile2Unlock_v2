//! ICredentialProviderEvents sink (Phase 1 skeleton).
//!
//! Phase 2 marshals the pointer handed to Provider::Advise to the worker
//! thread that talks to the auth service, and calls CredentialsChanged from
//! there when a credential becomes ready. Until then the sink is inert;
//! storing the raw interface pointer across threads without CoMarshalInterface
//! is forbidden (no unsafe Send/Sync bypass), so the Provider keeps only the
//! advise context.

use windows::Win32::UI::Shell::{ICredentialProviderEvents, ICredentialProviderEvents_Impl};
use windows_core::implement;

#[implement(ICredentialProviderEvents)]
pub struct EventSink;

impl ICredentialProviderEvents_Impl for EventSink_Impl {
    fn CredentialsChanged(&self, _upadvisecontext: usize) -> windows_core::Result<()> {
        Ok(())
    }
}
