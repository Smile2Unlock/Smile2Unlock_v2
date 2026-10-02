//! An optional out-of-process status display. No rendering, input hooks or
//! blocking IPC runs on LogonUI's COM thread. The SYSTEM-only mapping contains
//! one packed display state and a heartbeat; it never contains an identity,
//! biometric evidence or credential.
use crate::auto_runtime::{AutoRuntime, PipeRecognitionTransport};
use std::ffi::OsString;
use std::os::windows::ffi::OsStringExt;
use std::os::windows::process::CommandExt;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Weak};
use std::time::Duration;
use windows::Win32::Foundation::{CloseHandle, HLOCAL, HMODULE, INVALID_HANDLE_VALUE, LocalFree};
use windows::Win32::Security::Authorization::{
    ConvertStringSecurityDescriptorToSecurityDescriptorW, SDDL_REVISION_1,
};
use windows::Win32::Security::{PSECURITY_DESCRIPTOR, SECURITY_ATTRIBUTES};
use windows::Win32::System::Com::CoCreateGuid;
use windows::Win32::System::LibraryLoader::{
    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    GetModuleFileNameW, GetModuleHandleExW,
};
use windows::Win32::System::Memory::{
    CreateFileMappingW, FILE_MAP_WRITE, MapViewOfFile, PAGE_READWRITE, UnmapViewOfFile,
};
use windows::Win32::System::SystemInformation::GetTickCount64;
use windows::Win32::System::Threading::CREATE_NO_WINDOW;
use windows_core::{PCWSTR, w};

type Runtime = AutoRuntime<PipeRecognitionTransport>;

#[repr(C)]
struct DisplayMemory {
    magic: u32,
    version: u32,
    state: AtomicU32,
    reserved: u32,
    heartbeat: AtomicU64,
}
const _: () = assert!(size_of::<DisplayMemory>() == 24);

pub struct StatusBridge(Arc<AtomicBool>);
impl StatusBridge {
    pub fn start(runtime: &Arc<Runtime>) -> Option<Self> {
        // COM probes and unit tests must not start a secure-desktop child.
        if cfg!(test)
            || !std::env::current_exe()
                .ok()?
                .file_name()?
                .eq_ignore_ascii_case("LogonUI.exe")
        {
            return None;
        }
        let stop = Arc::new(AtomicBool::new(false));
        let cancelled = Arc::clone(&stop);
        let runtime = Arc::downgrade(runtime);
        std::thread::Builder::new()
            .name("su-status-display".into())
            .spawn(move || {
                if let Err(error) = run(runtime, &cancelled) {
                    crate::log::cp_log(&format!("status display unavailable: {error}"));
                }
            })
            .ok()?;
        Some(Self(stop))
    }
}
impl Drop for StatusBridge {
    fn drop(&mut self) {
        self.0.store(true, Ordering::Release);
    }
}

fn host_path() -> windows_core::Result<PathBuf> {
    let mut module = HMODULE::default();
    let mut path = [0u16; 32768];
    // Address-based lookup remains correct if the DLL filename changes.
    unsafe {
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            PCWSTR(host_path as *const () as *const u16),
            &mut module,
        )?;
        let len = GetModuleFileNameW(Some(module), &mut path) as usize;
        if len == 0 || len >= path.len() {
            return Err(windows_core::Error::from_thread());
        }
        Ok(PathBuf::from(OsString::from_wide(&path[..len]))
            .with_file_name("Smile2UnlockStatus.exe"))
    }
}

fn run(runtime: Weak<Runtime>, stop: &AtomicBool) -> Result<(), Box<dyn std::error::Error>> {
    let host = host_path()?;
    let name = format!(
        "Local\\Smile2Unlock.Status.{}.{:?}",
        std::process::id(),
        unsafe { CoCreateGuid()? }
    );
    let wide: Vec<u16> = name.encode_utf16().chain(Some(0)).collect();
    let mut descriptor = PSECURITY_DESCRIPTOR::default();
    unsafe {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            w!("D:P(A;;GA;;;SY)"),
            SDDL_REVISION_1,
            &mut descriptor,
            None,
        )?;
    }
    let attrs = SECURITY_ATTRIBUTES {
        nLength: size_of::<SECURITY_ATTRIBUTES>() as u32,
        lpSecurityDescriptor: descriptor.0,
        bInheritHandle: false.into(),
    };
    let mapping = unsafe {
        CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            Some(&attrs),
            PAGE_READWRITE,
            0,
            size_of::<DisplayMemory>() as u32,
            PCWSTR(wide.as_ptr()),
        )
    };
    unsafe {
        LocalFree(Some(HLOCAL(descriptor.0)));
    }
    let mapping = mapping?;
    let view = unsafe { MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, size_of::<DisplayMemory>()) };
    if view.Value.is_null() {
        unsafe {
            let _ = CloseHandle(mapping);
        }
        return Err(windows_core::Error::from_thread().into());
    }
    // Both processes access the packed state/heartbeat using aligned Windows
    // interlocked-compatible 32/64-bit atomics. The header is immutable.
    let data = view.Value.cast::<DisplayMemory>();
    unsafe {
        data.write(DisplayMemory {
            magic: 0x53325553,
            version: 1,
            state: AtomicU32::new(0),
            reserved: 0,
            heartbeat: AtomicU64::new(GetTickCount64()),
        });
    }
    let result = (|| -> Result<(), Box<dyn std::error::Error>> {
        let mut child = Command::new(host)
            .arg(&name)
            .arg(std::process::id().to_string())
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .creation_flags(CREATE_NO_WINDOW.0)
            .spawn()?;
        crate::log::cp_log(&format!("status display started pid={}", child.id()));
        while !stop.load(Ordering::Acquire) {
            let Some(runtime) = runtime.upgrade() else {
                break;
            };
            let state = runtime.display_snapshot();
            drop(runtime);
            unsafe {
                (*data).state.store(state, Ordering::Release);
                (*data).heartbeat.store(GetTickCount64(), Ordering::Release);
            }
            if child.try_wait()?.is_some() {
                break;
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        unsafe {
            (*data).state.store(0, Ordering::Release);
        }
        let _ = child.kill();
        let _ = child.wait();
        Ok(())
    })();
    unsafe {
        let _ = UnmapViewOfFile(view);
        let _ = CloseHandle(mapping);
    }
    result
}
