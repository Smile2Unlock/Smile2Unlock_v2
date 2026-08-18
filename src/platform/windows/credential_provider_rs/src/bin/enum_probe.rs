//! enum_probe — reproduce LogonUI's provider enumeration against the
//! registry-registered CLSID on a real Windows machine.
//!
//! LogonUI silently discards a credential without calling any credential
//! method when one of its interface checks fails. This probe replays the
//! enumeration sequence (CoCreateInstance -> SetUsageScenario -> QI
//! SetUserArray -> GetCredentialCount -> GetCredentialAt -> QI
//! ICredentialProviderCredential2 -> QI WithFieldOptions) against the
//! REGISTERED CLSID so each step's result is visible. Run on the target
//! Windows VM next to the deployed DLL.

use windows::Win32::System::Com::{
    CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED, CoCreateInstance, CoInitializeEx,
};
use windows::Win32::UI::Shell::{
    CPUS_LOGON, ICredentialProvider, ICredentialProviderCredential2,
    ICredentialProviderCredentialWithFieldOptions, ICredentialProviderSetUserArray,
};
use windows_core::{BOOL, GUID, Interface};

const CLSID_SU_PROVIDER: GUID = GUID {
    data1: 0x5fd3d285,
    data2: 0x0dd9,
    data3: 0x4362,
    data4: [0x88, 0x55, 0xe0, 0xab, 0xaa, 0xcd, 0x4a, 0xf6],
};

fn main() {
    unsafe {
        let _ = CoInitializeEx(None, COINIT_APARTMENTTHREADED);
    }
    println!("[probe] CoInitializeEx done");

    let prov: ICredentialProvider = match unsafe {
        CoCreateInstance::<_, ICredentialProvider>(&CLSID_SU_PROVIDER, None, CLSCTX_INPROC_SERVER)
    } {
        Ok(p) => {
            println!("[probe] CoCreateInstance(CLSID_SU_PROVIDER) OK");
            p
        }
        Err(e) => {
            println!("[probe] CoCreateInstance FAILED {:08x}", e.code().0);
            return;
        }
    };

    match unsafe { prov.SetUsageScenario(CPUS_LOGON, 0) } {
        Ok(_) => println!("[probe] SetUsageScenario(CPUS_LOGON) OK"),
        Err(e) => println!("[probe] SetUsageScenario FAILED {:08x}", e.code().0),
    }

    match prov.cast::<ICredentialProviderSetUserArray>() {
        Ok(_) => println!("[probe] QI(ICredentialProviderSetUserArray) OK"),
        Err(e) => println!("[probe] QI(SetUserArray) FAILED {:08x}", e.code().0),
    }

    let mut count = 0u32;
    let mut def = 0u32;
    let mut auto = BOOL(1);
    match unsafe { prov.GetCredentialCount(&mut count, &mut def, &mut auto) } {
        Ok(_) => println!(
            "[probe] GetCredentialCount count={} default={} autoLogon={}",
            count, def, auto.0
        ),
        Err(e) => println!("[probe] GetCredentialCount FAILED {:08x}", e.code().0),
    }

    match unsafe { prov.GetCredentialAt(0) } {
        Ok(cred) => {
            println!("[probe] GetCredentialAt(0) OK");
            match cred.cast::<ICredentialProviderCredential2>() {
                Ok(c2) => {
                    println!("[probe] QI(ICredentialProviderCredential2) OK");
                    match unsafe { c2.GetUserSid() } {
                        Ok(sid) => {
                            if sid.0.is_null() {
                                // S_FALSE + NULL (empty tile association) is
                                // the C++ baseline contract for no-user; do
                                // NOT call to_string on a null PWSTR.
                                println!("[probe] GetUserSid -> <null> (S_FALSE empty tile)");
                            } else {
                                let text = unsafe { sid.to_string() }.unwrap_or_default();
                                println!("[probe] GetUserSid -> '{}'", text);
                                unsafe {
                                    windows::Win32::System::Com::CoTaskMemFree(Some(
                                        sid.0 as *const core::ffi::c_void,
                                    ))
                                };
                            }
                        }
                        Err(e) => println!("[probe] GetUserSid FAILED {:08x}", e.code().0),
                    }
                }
                Err(e) => println!("[probe] QI(Credential2) FAILED {:08x}", e.code().0),
            }
            match cred.cast::<ICredentialProviderCredentialWithFieldOptions>() {
                Ok(wfo) => {
                    println!("[probe] QI(WithFieldOptions) OK");
                    match unsafe { wfo.GetFieldOptions(3) } {
                        Ok(o) => println!("[probe] GetFieldOptions(3) -> {}", o.0),
                        Err(e) => println!("[probe] GetFieldOptions FAILED {:08x}", e.code().0),
                    }
                }
                Err(e) => println!("[probe] QI(WithFieldOptions) FAILED {:08x}", e.code().0),
            }
        }
        Err(e) => println!("[probe] GetCredentialAt FAILED {:08x}", e.code().0),
    }

    println!("[probe] done");
}
