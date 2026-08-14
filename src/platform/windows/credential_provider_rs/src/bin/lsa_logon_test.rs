//! lsa_logon_test — SYSTEM-only tool that feeds the exact serialization the
//! credential provider produces into LsaLogonUser, so the LSA verdict on the
//! packed credentials is visible directly (LogonUI never reports it).
//!
//! Run via Task Scheduler as SYSTEM:
//!   lsa_logon_test.exe <request_id>

use su_credential_provider::{kerb_interactive_unlock_logon_init, kerb_interactive_unlock_logon_pack, protect_password};
use windows::Win32::Foundation::{HANDLE, LUID};
use windows::Win32::Security::Authentication::Identity::{
    LsaConnectUntrusted, LsaDeregisterLogonProcess, LsaLogonUser, LsaLookupAuthenticationPackage,
    SECURITY_LOGON_TYPE,
};
use windows::Win32::Security::{TOKEN_GROUPS, TOKEN_SOURCE};
use windows::Win32::Security::Authentication::Identity::LSA_STRING;
use windows::Win32::Foundation::CloseHandle;
use windows::Win32::Security::QUOTA_LIMITS;
use windows_core::PSTR;

const NEGOSSP_NAME_A: &[u8] = b"Negotiate";

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let request_id: u64 = args.get(1).and_then(|v| v.parse().ok()).unwrap_or(77);

    // 1. fetch the stored password through the pipe (same as GetSerialization)
    let sid: Vec<u16> = "S-1-5-21-2028198983-2841916586-3191050802-500"
        .encode_utf16()
        .collect();
    let mut password = match su_credential_provider::PipeClient.prepare(&sid, request_id, 0) {
        Ok(pw) => {
            println!("[lsa] pipe prepare OK");
            pw
        }
        Err(e) => {
            println!("[lsa] pipe prepare FAILED {:08x}", e.code().0);
            return;
        }
    };

    // 2. protect the password exactly like GetSerialization
    let protected = match password.with_password(|units| protect_password(units)) {
        Ok(Ok(p)) => p,
        Ok(Err(e)) => {
            println!("[lsa] protect_password FAILED {:08x}", e.code().0);
            return;
        }
        Err(e) => {
            println!("[lsa] secret view FAILED {:?}", e);
            return;
        }
    };

    // 3. build the manual KERB serialization (C++ parity)
    let domain: Vec<u16> = "DESKTOP-308FE27".encode_utf16().collect();
    let username: Vec<u16> = "Administrator".encode_utf16().collect();
    let kiul = match kerb_interactive_unlock_logon_init(&domain, &username, &protected, 1) {
        Ok(k) => k,
        Err(e) => {
            println!("[lsa] kerb init FAILED {:08x}", e.code().0);
            return;
        }
    };
    let (blob, blob_len) = unsafe { kerb_interactive_unlock_logon_pack(&kiul) }.unwrap();
    println!("[lsa] serialization cb={}", blob_len);

    // 4. LSA logon
    let mut handle = HANDLE::default();
    let status = unsafe { LsaConnectUntrusted(&mut handle) };
    println!("[lsa] LsaConnectUntrusted nt={:#x}", status.0);
    if status.0 != 0 {
        return;
    }

    let mut package_name = LSA_STRING {
        Length: NEGOSSP_NAME_A.len() as u16,
        MaximumLength: (NEGOSSP_NAME_A.len() + 1) as u16,
        Buffer: PSTR::from_raw(NEGOSSP_NAME_A.as_ptr() as *mut u8),
    };
    let mut package = 0u32;
    let status = unsafe { LsaLookupAuthenticationPackage(handle, &mut package_name, &mut package) };
    println!("[lsa] LsaLookupAuthenticationPackage nt={:#x} id={}", status.0, package);
    if status.0 != 0 {
        unsafe { let _ = LsaDeregisterLogonProcess(handle); };
        return;
    }

    let mut origin = LSA_STRING {
        Length: 6,
        MaximumLength: 7,
        Buffer: PSTR::from_raw(b"su_test".as_ptr() as *mut u8),
    };
    let mut source = TOKEN_SOURCE {
        SourceName: [0; 8],
        SourceIdentifier: LUID::default(),
    };
    let name: [i8; 8] = [b's' as i8, b'u' as i8, b'_' as i8, b't' as i8, b'e' as i8, b's' as i8, b't' as i8, 0];
    source.SourceName = name;
    let mut profile: *mut core::ffi::c_void = core::ptr::null_mut();
    let mut profile_len: u32 = 0;
    let mut logon_id = LUID::default();
    let mut token = HANDLE::default();
    let mut quotas = QUOTA_LIMITS::default();
    let mut substatus: i32 = 0;

    let status = unsafe {
        LsaLogonUser(
            handle,
            &origin,
            SECURITY_LOGON_TYPE(2), // Interactive
            package,
            blob as *const core::ffi::c_void,
            blob_len as u32,
            None,
            &source,
            &mut profile,
            &mut profile_len,
            &mut logon_id,
            &mut token,
            &mut quotas,
            &mut substatus,
        )
    };
    println!("[lsa] LsaLogonUser nt={:#x} substatus={:#x}", status.0, substatus);
    if status.0 == 0 {
        println!("[lsa] LOGON SUCCESS! token={:?}", token);
        unsafe {
            let _ = CloseHandle(token);
        }
    }

    unsafe { let _ = LsaDeregisterLogonProcess(handle); };
    unsafe { windows::Win32::System::Com::CoTaskMemFree(Some(blob as *const _)) };
    println!("[lsa] done");
}
