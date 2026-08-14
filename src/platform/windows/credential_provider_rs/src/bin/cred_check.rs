//! cred_check — verify the serialization primitives on a real Windows box:
//!  1. CredProtectW("pwd") -> CredUnprotectW round trip
//!  2. LsaLookupAuthenticationPackage("Negotiate") id
//!  3. CredPackAuthenticationBufferW(CRED_PACK_PROTECTED_CREDENTIALS) size

use su_credential_provider::{
    cred_pack_authentication_buffer, protect_password, retrieve_negotiate_auth_package,
};
use windows::Win32::Security::Credentials::{CredUnprotectW, CRED_PROTECTION_TYPE};
use windows_core::PWSTR;

fn main() {
    // 1. CredProtect round trip
    let pw: Vec<u16> = "pwd".encode_utf16().collect();
    match protect_password(&pw) {
        Ok(protected) => {
            println!("[check] protect_password OK len={}", protected.len());
            let mut protected_nul = protected.clone();
            protected_nul.push(0);
            let mut cch = 0u32;
            let _ = unsafe { CredUnprotectW(false, &protected_nul, None, &mut cch) };
            if cch > 0 && cch < 1024 {
                let mut plain = vec![0u16; cch as usize];
                let r = unsafe {
                    CredUnprotectW(
                        false,
                        &protected_nul,
                        Some(PWSTR::from_raw(plain.as_mut_ptr())),
                        &mut cch,
                    )
                };
                match r {
                    Ok(()) => {
                        plain.truncate(cch as usize);
                        let text = String::from_utf16_lossy(&plain);
                        println!("[check] CredUnprotect OK -> '{}'", text);
                    }
                    Err(e) => println!("[check] CredUnprotect FAILED {:08x}", e.code().0),
                }
            } else {
                println!("[check] CredUnprotect probe cch={}", cch);
            }
        }
        Err(e) => println!("[check] protect_password FAILED {:08x}", e.code().0),
    }

    // 2. Negotiate package id
    match retrieve_negotiate_auth_package() {
        Ok(id) => println!("[check] negotiate package id={}", id),
        Err(e) => println!("[check] negotiate lookup FAILED {:08x}", e.code().0),
    }

    // 3. CredPack size with a real protected password
    let qualified: Vec<u16> = "DESKTOP-308FE27\\Administrator".encode_utf16().collect();
    match protect_password(&pw) {
        Ok(protected) => match unsafe { cred_pack_authentication_buffer(&qualified, &protected) } {
            Ok((ptr, cb)) => {
                println!("[check] cred_pack cb={}", cb);
                unsafe { windows::Win32::System::Com::CoTaskMemFree(Some(ptr as *const _)) };
            }
            Err(e) => println!("[check] cred_pack FAILED {:08x}", e.code().0),
        },
        Err(_) => {}
    }
    let _ = CRED_PROTECTION_TYPE::default();
}
