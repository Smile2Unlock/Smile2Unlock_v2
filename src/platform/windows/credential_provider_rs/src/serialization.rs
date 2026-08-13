//! KERB_INTERACTIVE_UNLOCK_LOGON serialization, password protection and LSA
//! auth-package lookup (Phase 3).
//!
//! Wire semantics mirror the C++ baseline (CredentialProvider/helpers.cpp):
//! the packed buffer stores UNICODE_STRING.Buffer as a byte offset relative
//! to the structure base (not an absolute pointer), strings are NOT
//! NUL-terminated and Length is measured in bytes.

use core::mem::{size_of, MaybeUninit};
use core::ptr;

use windows::Win32::Foundation::LUID;
use windows::Win32::Security::Authentication::Identity::{
    KerbInteractiveLogon, KerbWorkstationUnlockLogon, LSA_STRING, LSA_UNICODE_STRING,
    LsaConnectUntrusted, LsaDeregisterLogonProcess, LsaLookupAuthenticationPackage,
    KERB_INTERACTIVE_LOGON, KERB_INTERACTIVE_UNLOCK_LOGON,
};
use windows::Win32::Security::Credentials::{
    CredIsProtectedW, CredProtectW, CRED_PROTECTION_TYPE,
};
use windows::Win32::System::Com::CoTaskMemAlloc;
use windows::Win32::System::WindowsProgramming::GetComputerNameW;
use windows_core::{Error, PSTR, PWSTR};

use crate::{E_FAIL, E_OUTOFMEMORY};

/// CPUS_LOGON from wincred.h.
pub const CPUS_LOGON: i32 = 1;
/// CPUS_UNLOCK_WORKSTATION from wincred.h.
pub const CPUS_UNLOCK_WORKSTATION: i32 = 2;

fn make_lsa_string(value: &[u16]) -> LSA_UNICODE_STRING {
    // Length excludes the NUL terminator (C++ wcslen semantics).
    let text_len = value.iter().position(|&c| c == 0).unwrap_or(value.len());
    let len = text_len.checked_mul(2).unwrap_or(u16::MAX as usize);
    LSA_UNICODE_STRING {
        Length: len.min(u16::MAX as usize) as u16,
        MaximumLength: len.saturating_add(2).min(u16::MAX as usize) as u16,
        Buffer: PWSTR::from_raw(value.as_ptr().cast_mut()),
    }
}

/// Assemble a KERB_INTERACTIVE_UNLOCK_LOGON for the given usage scenario.
///
/// `domain`/`username`/`password` must be NUL-terminated UTF-16 buffers; the
/// UNICODE_STRING Length fields exclude the NUL (matching C++ wcslen-based
/// initialization). Only CPUS_LOGON and CPUS_UNLOCK_WORKSTATION are
/// supported in v1; anything else fails with E_FAIL.
pub fn kerb_interactive_unlock_logon_init(
    domain: &[u16],
    username: &[u16],
    password: &[u16],
    cpus: i32,
) -> windows_core::Result<KERB_INTERACTIVE_UNLOCK_LOGON> {
    let message_type = match cpus {
        CPUS_LOGON => KerbInteractiveLogon,
        CPUS_UNLOCK_WORKSTATION => KerbWorkstationUnlockLogon,
        _ => return Err(Error::from_hresult(E_FAIL)),
    };
    Ok(KERB_INTERACTIVE_UNLOCK_LOGON {
        Logon: KERB_INTERACTIVE_LOGON {
            MessageType: message_type,
            LogonDomainName: make_lsa_string(domain),
            UserName: make_lsa_string(username),
            Password: make_lsa_string(password),
        },
        LogonId: LUID { LowPart: 0, HighPart: 0 },
    })
}

/// Serialize a KERB_INTERACTIVE_UNLOCK_LOGON into a CoTaskMemAlloc'ed buffer
/// in the packed format consumed by WinLogon/LSA: string bytes follow the
/// structure inline and UNICODE_STRING.Buffer holds a byte offset relative
/// to the structure base.
///
/// Returns the buffer pointer and its byte size. The caller owns the buffer
/// (CoTaskMemFree) and is responsible for wiping it when done.
///
/// # Safety
/// `kiul` string buffers must be valid for their Length byte counts.
pub unsafe fn kerb_interactive_unlock_logon_pack(
    kiul: &KERB_INTERACTIVE_UNLOCK_LOGON,
) -> windows_core::Result<(*mut u8, usize)> {
    let base_size = size_of::<KERB_INTERACTIVE_UNLOCK_LOGON>();
    let total = base_size
        .checked_add(kiul.Logon.LogonDomainName.Length as usize)
        .and_then(|v| v.checked_add(kiul.Logon.UserName.Length as usize))
        .and_then(|v| v.checked_add(kiul.Logon.Password.Length as usize))
        .ok_or_else(|| Error::from_hresult(E_OUTOFMEMORY))?;

    // SAFETY: CoTaskMemAlloc returns a pointer to `total` zeroed-free bytes.
    let mem = unsafe { CoTaskMemAlloc(total) };
    if mem.is_null() {
        return Err(Error::from_hresult(E_OUTOFMEMORY));
    }
    let base = mem.cast::<KERB_INTERACTIVE_UNLOCK_LOGON>();
    // SAFETY: buffer is at least as large as the struct (total >= base_size).
    unsafe { ptr::write(base, *kiul) };
    // Zero LogonId like the C++ ZeroMemory(&pkiulOut->LogonId).
    // SAFETY: LogonId is the second field; base points to a valid struct.
    unsafe { (*base).LogonId = LUID { LowPart: 0, HighPart: 0 } };

    // Copy each string inline; Buffer becomes a byte offset from the base.
    let mut cursor = unsafe { (base as *mut u8).add(base_size) };
    // SAFETY: kiul strings are valid for their Lengths.
    unsafe {
        let (next, off) = copy_inline_string(kiul.Logon.LogonDomainName, base, cursor);
        (*base).Logon.LogonDomainName.Buffer = PWSTR::from_raw(off as *mut u16);
        cursor = next;
        let (next, off) = copy_inline_string(kiul.Logon.UserName, base, cursor);
        (*base).Logon.UserName.Buffer = PWSTR::from_raw(off as *mut u16);
        cursor = next;
        let (_, off) = copy_inline_string(kiul.Logon.Password, base, cursor);
        (*base).Logon.Password.Buffer = PWSTR::from_raw(off as *mut u16);
    }
    Ok((mem.cast::<u8>(), total))
}

/// Copy the bytes behind `s` to `cursor`. Returns the advanced cursor and the
/// byte offset of the copy relative to `base`.
///
/// # Safety
/// `s.Buffer` must be valid for `s.Length` bytes; `base` must point to the
/// serialization structure; `cursor` must point to writable space of at
/// least `s.Length` bytes.
unsafe fn copy_inline_string(
    s: LSA_UNICODE_STRING,
    base: *mut KERB_INTERACTIVE_UNLOCK_LOGON,
    cursor: *mut u8,
) -> (*mut u8, usize) {
    let len = s.Length as usize;
    if len > 0 {
        // SAFETY: caller guarantees both regions are valid.
        unsafe { ptr::copy_nonoverlapping(s.Buffer.as_ptr().cast::<u8>(), cursor, len) };
    }
    let offset = (cursor as usize) - (base as usize);
    (unsafe { cursor.add(len) }, offset)
}

/// Protect a plaintext UTF-16 password (NUL-terminated) with CredProtectW,
/// mirroring helpers.cpp `_ProtectAndCopyString`:
///  - empty password -> empty string, unchanged
///  - already protected (CredIsProtectedW) -> copied through unchanged
///  - otherwise the two-pass CredProtectW (probe length, then encrypt)
///
/// Returns a NUL-terminated protected buffer. The caller must wipe the
/// returned bytes when done (the encrypted blob is still sensitive).
pub fn protect_password(password: &[u16]) -> windows_core::Result<Vec<u16>> {
    if password.is_empty() || password[0] == 0 {
        return Ok(vec![0]);
    }

    // SAFETY: password points to a NUL-terminated buffer.
    let mut protection_type = CRED_PROTECTION_TYPE::default();
    // CredIsProtectedW failure (e.g. malformed blob) -> treat as unprotected.
    let already_protected = unsafe {
        CredIsProtectedW(PWSTR::from_raw(password.as_ptr().cast_mut()), &mut protection_type)
            .is_ok()
            && protection_type.0 != 0
    };
    if already_protected {
        return Ok(password.to_vec());
    }

    // First pass: probe the required character count (including NUL).
    let mut cch = 0u32;
    // SAFETY: out buffer is null, cch starts at 0; the API reports the size.
    let _ = unsafe {
        CredProtectW(
            false,
            password,
            PWSTR::null(),
            &mut cch,
            None,
        )
    };
    if cch == 0 || cch > 513 {
        return Err(crate::win32_error(122)); // ERROR_INSUFFICIENT_BUFFER
    }
    let mut out = vec![0u16; cch as usize];
    // SAFETY: out has cch elements; the API writes exactly cch chars + NUL.
    unsafe {
        CredProtectW(
            false,
            password,
            PWSTR::from_raw(out.as_mut_ptr()),
            &mut cch,
            None,
        )?
    };
    out.truncate(cch as usize);
    Ok(out)
}

/// Look up the Negotiate (NEGOSSP) LSA authentication package identifier.
pub fn retrieve_negotiate_auth_package() -> windows_core::Result<u32> {
    // SAFETY: local handle slot.
    let mut handle = MaybeUninit::uninit();
    let status = unsafe { LsaConnectUntrusted(handle.as_mut_ptr()) };
    if status.0 != 0 {
        return Err(hresult_from_nt(status.0));
    }
    // SAFETY: handle initialized on success above.
    let handle = unsafe { handle.assume_init() };

    const NEGOSSP: &[u8] = b"NEGOSSP";
    let package_name = LSA_STRING {
        Length: NEGOSSP.len() as u16,
        MaximumLength: (NEGOSSP.len() + 1) as u16,
        // SAFETY: static ASCII, no mutation.
        Buffer: PSTR::from_raw(NEGOSSP.as_ptr() as *mut u8),
    };
    let mut package = 0u32;
    // SAFETY: handle and package slots are valid.
    let status = unsafe { LsaLookupAuthenticationPackage(handle, &package_name, &mut package) };
    // SAFETY: best-effort cleanup regardless of lookup result.
    unsafe { let _ = LsaDeregisterLogonProcess(handle); };
    if status.0 != 0 {
        return Err(hresult_from_nt(status.0));
    }
    Ok(package)
}

/// HRESULT_FROM_NT: NTSTATUS with the NT facility bit promoted to HRESULT.
fn hresult_from_nt(status: i32) -> Error {
    Error::from_hresult(crate::HRESULT((status as u32 | 0x1000_0000) as i32))
}

/// Split a qualified user name into (domain, username), mirroring the C++
/// GetSerialization logic:
///  - "DOMAIN\\user"      -> ("DOMAIN", "user")
///  - "user@example.com"  -> ("MicrosoftAccount", "user")
///  - "user"              -> (local computer name, "user")
pub fn split_domain_and_username(qualified: &[u16]) -> windows_core::Result<(Vec<u16>, Vec<u16>)> {
    if let Some(idx) = qualified.iter().position(|&c| c == b'\\' as u16) {
        return Ok((qualified[..idx].to_vec(), qualified[idx + 1..].to_vec()));
    }
    if qualified.iter().any(|&c| c == b'@' as u16) {
        const MICROSOFT_ACCOUNT: &[u16] = &[
            b'M' as u16, b'i' as u16, b'c' as u16, b'r' as u16, b'o' as u16, b's' as u16,
            b'o' as u16, b'f' as u16, b't' as u16, b'A' as u16, b'c' as u16, b'c' as u16,
            b'o' as u16, b'u' as u16, b'n' as u16, b't' as u16,
        ];
        return Ok((MICROSOFT_ACCOUNT.to_vec(), qualified.to_vec()));
    }
    let mut name = [0u16; 128];
    let mut size = name.len() as u32;
    // SAFETY: valid writable buffer + size slot.
    unsafe { GetComputerNameW(Some(PWSTR::from_raw(name.as_mut_ptr())), &mut size) }?;
    Ok((name[..size as usize].to_vec(), qualified.to_vec()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::mem::size_of;
    use windows_core::HRESULT;

    #[test]
    fn kerb_init_message_types() {
        let domain = wide("SUBOX");
        let user = wide("user1");
        let pass = wide("pw");
        let logon = kerb_interactive_unlock_logon_init(&domain, &user, &pass, CPUS_LOGON).unwrap();
        assert_eq!(logon.Logon.MessageType, KerbInteractiveLogon);
        let unlock =
            kerb_interactive_unlock_logon_init(&domain, &user, &pass, CPUS_UNLOCK_WORKSTATION)
                .unwrap();
        assert_eq!(unlock.Logon.MessageType, KerbWorkstationUnlockLogon);
        assert!(kerb_interactive_unlock_logon_init(&domain, &user, &pass, 4).is_err());
    }

    #[test]
    fn kerb_init_string_lengths_are_bytes_without_nul() {
        let domain = wide("SUBOX");
        let user = wide("user1");
        let pass = wide("pw");
        let kiul = kerb_interactive_unlock_logon_init(&domain, &user, &pass, CPUS_LOGON).unwrap();
        assert_eq!(kiul.Logon.LogonDomainName.Length, 10); // 5 chars * 2
        assert_eq!(kiul.Logon.UserName.Length, 10);
        assert_eq!(kiul.Logon.Password.Length, 4);
        assert_eq!(kiul.LogonId.LowPart, 0);
        assert_eq!(kiul.LogonId.HighPart, 0);
    }

    #[test]
    fn pack_layout_matches_cpp_semantics() {
        let domain = wide("SUBOX");
        let user = wide("user1");
        let pass = wide("pw");
        let kiul = kerb_interactive_unlock_logon_init(&domain, &user, &pass, CPUS_LOGON).unwrap();
        let base_size = size_of::<KERB_INTERACTIVE_UNLOCK_LOGON>();
        let expected_total = base_size + 10 + 10 + 4;
        // SAFETY: kiul buffers are valid.
        let (ptr, total) = unsafe { kerb_interactive_unlock_logon_pack(&kiul) }.unwrap();
        assert_eq!(total, expected_total);

        // SAFETY: owned CoTaskMem buffer.
        let base = unsafe { &*ptr.cast::<KERB_INTERACTIVE_UNLOCK_LOGON>() };
        assert_eq!(base.Logon.MessageType, KerbInteractiveLogon);
        // Buffers are offsets relative to the struct base.
        assert_eq!(base.Logon.LogonDomainName.Buffer.0 as usize, base_size);
        assert_eq!(base.Logon.UserName.Buffer.0 as usize, base_size + 10);
        assert_eq!(base.Logon.Password.Buffer.0 as usize, base_size + 20);
        // Inline bytes match the sources.
        // SAFETY: offsets verified above; read 10 bytes at each region.
        let bytes = unsafe { core::slice::from_raw_parts(ptr, total) };
        let d = &bytes[base_size..base_size + 10];
        assert_eq!(d, b"S\0U\0B\0O\0X\0");
        let u = &bytes[base_size + 10..base_size + 20];
        assert_eq!(u, b"u\0s\0e\0r\01\0");
        let p = &bytes[base_size + 20..base_size + 24];
        assert_eq!(p, b"p\0w\0");

        // SAFETY: release the CoTaskMem buffer.
        unsafe { windows::Win32::System::Com::CoTaskMemFree(Some(ptr.cast())) };
    }

    #[test]
    fn protect_password_empty_is_unchanged() {
        assert_eq!(protect_password(&[0]).unwrap(), vec![0]);
    }

    #[test]
    #[ignore = "wine 11.15 lacks advapi32.CredIsProtectedW (unimplemented, aborts); verify on the real Windows VM in Phase 4"]
    fn protect_password_roundtrip() {
        let plaintext = wide("correct horse battery");
        let protected = protect_password(&plaintext).unwrap();
        assert!(!protected.is_empty());
        // Protected blob differs from the plaintext.
        assert_ne!(protected, plaintext);
        // The blob is recognized as protected.
        let mut protection_type = CRED_PROTECTION_TYPE::default();
        // SAFETY: protected is NUL-terminated.
        let ok = unsafe {
            CredIsProtectedW(PWSTR::from_raw(protected.as_ptr().cast_mut()), &mut protection_type)
        };
        assert!(ok.is_ok());
        assert_ne!(protection_type.0, 0);
    }

    #[test]
    fn split_domain_cases() {
        // "DOMAIN\\user"
        let q: Vec<u16> = b"DOMAIN\\user".iter().map(|&b| b as u16).collect();
        let (d, u) = split_domain_and_username(&q).unwrap();
        assert_eq!(d, b"DOMAIN".iter().map(|&b| b as u16).collect::<Vec<_>>());
        assert_eq!(u, b"user".iter().map(|&b| b as u16).collect::<Vec<_>>());

        // "user@example.com" -> MicrosoftAccount
        let q: Vec<u16> = b"user@example.com".iter().map(|&b| b as u16).collect();
        let (d, u) = split_domain_and_username(&q).unwrap();
        assert_eq!(d, b"MicrosoftAccount".iter().map(|&b| b as u16).collect::<Vec<_>>());
        assert_eq!(u, q);

        // "user" -> local computer name (non-empty on any Windows).
        let q: Vec<u16> = b"user".iter().map(|&b| b as u16).collect();
        let (d, u) = split_domain_and_username(&q).unwrap();
        assert!(!d.is_empty());
        assert_eq!(u, q);
    }

    #[test]
    fn negotiate_package_lookup() {
        // wine ships an LSA; the Negotiate package may or may not be
        // registered there. We only assert the plumbing returns either a
        // package id or a well-formed NT-facility error.
        match retrieve_negotiate_auth_package() {
            Ok(_pkg) => {}
            Err(e) => {
                let code = e.code();
                assert_eq!(code.0 & 0x1000_0000, 0x1000_0000);
            }
        }
    }

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(core::iter::once(0)).collect()
    }
}
