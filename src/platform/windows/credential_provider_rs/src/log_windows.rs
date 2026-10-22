//! Effects for the LogonUI logger. No directories are created here.
use std::fs::{File, OpenOptions};
use std::io::Write;
use std::os::windows::fs::OpenOptionsExt;
use std::os::windows::io::AsRawHandle;
use std::path::{Path, PathBuf};
use windows::Win32::Foundation::{HANDLE, HLOCAL, LocalFree};
use windows::Win32::Security::Authorization::{GetSecurityInfo, SE_FILE_OBJECT};
use windows::Win32::Security::*;
use windows::Win32::Storage::FileSystem::*;
use windows::Win32::System::Com::CoTaskMemFree;
use windows::Win32::System::Registry::*;
use windows::Win32::System::Threading::GetCurrentThreadId;
use windows::Win32::System::Time::{GetTimeZoneInformation, TIME_ZONE_INFORMATION};
use windows::Win32::UI::Shell::{FOLDERID_ProgramData, KF_FLAG_DEFAULT, SHGetKnownFolderPath};
use windows::core::w;

pub fn path() -> Option<PathBuf> {
    // SAFETY: shell allocates a NUL-terminated string; it is released after copying.
    let raw = unsafe { SHGetKnownFolderPath(&FOLDERID_ProgramData, KF_FLAG_DEFAULT, None) }.ok()?;
    let path = unsafe { raw.to_string() }.ok().map(PathBuf::from);
    unsafe { CoTaskMemFree(Some(raw.0.cast())) };
    Some(
        path?
            .join("Smile2Unlock")
            .join("Logs")
            .join("credential-provider.log"),
    )
}
pub fn registry_level() -> Option<String> {
    let mut value = [0u16; 64];
    let mut size = size_of_val(&value) as u32;
    // SAFETY: bounded output buffer; the registry API terminates REG_SZ results.
    let status = unsafe {
        RegGetValueW(
            HKEY_LOCAL_MACHINE,
            w!("SOFTWARE\\Smile2Unlock"),
            w!("LogLevel"),
            RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
            None,
            Some(value.as_mut_ptr().cast()),
            Some(&mut size),
        )
    };
    if status.0 == 2 || status.0 == 3 {
        return None;
    }
    if status.0 != 0 {
        return Some("invalid".into());
    }
    let length = value.iter().position(|c| *c == 0).unwrap_or(value.len());
    Some(String::from_utf16_lossy(&value[..length]))
}
pub fn thread_and_offset() -> (u32, i32) {
    let mut zone = TIME_ZONE_INFORMATION::default();
    // SAFETY: initialized output buffer, no borrowed pointers are retained.
    let state = unsafe { GetTimeZoneInformation(&mut zone) };
    let bias = if state == 2 {
        zone.DaylightBias
    } else if state == 1 {
        zone.StandardBias
    } else {
        0
    };
    (
        unsafe { GetCurrentThreadId() },
        if state == u32::MAX {
            0
        } else {
            -zone.Bias - bias
        },
    )
}
fn trusted(sid: PSID) -> bool {
    !sid.0.is_null()
        && unsafe {
            IsWellKnownSid(sid, WinLocalSystemSid).as_bool()
                || IsWellKnownSid(sid, WinBuiltinAdministratorsSid).as_bool()
        }
}
fn security(file: &File, directory: bool) -> Option<()> {
    let mut owner = PSID::default();
    let mut dacl = std::ptr::null_mut();
    let mut descriptor = PSECURITY_DESCRIPTOR::default();
    // SAFETY: handle remains alive; GetSecurityInfo allocates descriptor, which owns SID/ACL.
    let status = unsafe {
        GetSecurityInfo(
            HANDLE(file.as_raw_handle()),
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            Some(&mut owner),
            None,
            Some(&mut dacl),
            None,
            Some(&mut descriptor),
        )
    };
    let mut safe = status.0 == 0 && trusted(owner);
    if safe && directory {
        safe = safe_acl(dacl);
    }
    if !descriptor.0.is_null() {
        unsafe {
            let _ = LocalFree(Some(HLOCAL(descriptor.0)));
        }
    }
    safe.then_some(())
}
fn safe_acl(dacl: *mut ACL) -> bool {
    if dacl.is_null() {
        return false;
    }
    // SAFETY: dacl belongs to a successful GetSecurityInfo result for this call.
    for index in 0..unsafe { (*dacl).AceCount } {
        let mut ace = std::ptr::null_mut();
        if unsafe { GetAce(dacl, index as u32, &mut ace) }.is_err() {
            return false;
        }
        let allowed = ace.cast::<ACCESS_ALLOWED_ACE>();
        if unsafe { (*allowed).Header.AceType } != 0 {
            return false;
        }
        if !trusted(PSID(
            unsafe { std::ptr::addr_of_mut!((*allowed).SidStart) }.cast(),
        )) {
            return false;
        }
    }
    true
}
fn checked(path: &Path, directory: bool, system: bool, create: bool) -> Option<File> {
    let file = OpenOptions::new()
        .append(!directory)
        .read(directory)
        .access_mode(0x00020080 | if directory { 0 } else { 4 })
        .create(create)
        .share_mode(if directory {
            FILE_SHARE_READ.0 | FILE_SHARE_WRITE.0
        } else {
            FILE_SHARE_READ.0 | FILE_SHARE_WRITE.0 | FILE_SHARE_DELETE.0
        })
        .custom_flags(
            FILE_FLAG_OPEN_REPARSE_POINT.0
                | if directory {
                    FILE_FLAG_BACKUP_SEMANTICS.0
                } else {
                    0
                },
        )
        .open(path)
        .ok()?;
    let mut info = FILE_ATTRIBUTE_TAG_INFO::default();
    // SAFETY: live handle, correctly sized initialized output buffer.
    unsafe {
        GetFileInformationByHandleEx(
            HANDLE(file.as_raw_handle()),
            FileAttributeTagInfo,
            (&mut info as *mut FILE_ATTRIBUTE_TAG_INFO).cast(),
            size_of_val(&info) as u32,
        )
    }
    .ok()?;
    if info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT.0 != 0
        || (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY.0 != 0) != directory
    {
        return None;
    }
    if system {
        security(&file, directory)?;
    }
    Some(file)
}
fn parents(path: &Path, system: bool) -> Option<Vec<File>> {
    let mut current = PathBuf::new();
    let mut product = false;
    let mut guards = Vec::new();
    for part in path.parent()?.components() {
        current.push(part);
        if !matches!(part, std::path::Component::Normal(_)) {
            continue;
        }
        product |= part.as_os_str() == "Smile2Unlock";
        guards.push(checked(&current, true, system && product, false)?);
    }
    Some(guards)
}
fn rotate(path: &Path, system: bool) -> Option<()> {
    for index in (1..=3).rev() {
        let source = if index == 1 {
            path.to_path_buf()
        } else {
            path.with_extension(format!("log.{}", index - 1))
        };
        let destination = path.with_extension(format!("log.{index}"));
        match std::fs::symlink_metadata(&source) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
            Err(_) => return None,
            Ok(_) => {
                checked(&source, false, system, false)?;
            }
        }
        match std::fs::symlink_metadata(&destination) {
            Ok(_) => {
                checked(&destination, false, system, false)?;
                std::fs::remove_file(&destination).ok()?;
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => (),
            Err(_) => return None,
        }
        std::fs::rename(source, destination).ok()?;
    }
    Some(())
}
fn open_current(path: &Path, system: bool) -> Option<(File, Vec<File>)> {
    // Pin each checked ancestor until this append handle is replaced.
    let guards = parents(path, system)?;
    Some((checked(path, false, system, true)?, guards))
}
fn same_file(first: &File, second: &File) -> Option<bool> {
    let mut a = BY_HANDLE_FILE_INFORMATION::default();
    let mut b = BY_HANDLE_FILE_INFORMATION::default();
    // SAFETY: live file handles and correctly sized initialized output buffers.
    unsafe {
        GetFileInformationByHandle(HANDLE(first.as_raw_handle()), &mut a).ok()?;
        GetFileInformationByHandle(HANDLE(second.as_raw_handle()), &mut b).ok()?;
    }
    Some(
        a.dwVolumeSerialNumber == b.dwVolumeSerialNumber
            && a.nFileIndexHigh == b.nFileIndexHigh
            && a.nFileIndexLow == b.nFileIndexLow,
    )
}
#[derive(Default)]
pub struct Writer {
    file: Option<File>,
    guards: Vec<File>,
}
impl Writer {
    fn prepare_rotation(&mut self, path: &Path, system: bool) -> Option<()> {
        let (current, guards) = open_current(path, system)?;
        self.guards = guards;
        if same_file(self.file.as_ref()?, &current)? {
            // Keep the original append handle on any rename/reopen failure.
            rotate(path, system)?;
            let (next, guards) = open_current(path, system)?;
            self.file = Some(next);
            self.guards = guards;
        } else {
            // Another process rotated our handle; join its current file.
            self.file = Some(current);
        }
        Some(())
    }
    fn append_with_limit(
        &mut self,
        path: &Path,
        line: &str,
        system: bool,
        limit: u64,
    ) -> Option<()> {
        if self.file.is_none() {
            let (file, guards) = open_current(path, system)?;
            self.file = Some(file);
            self.guards = guards;
        }
        let size = self.file.as_ref()?.metadata().ok()?.len();
        if size > 0 && size.saturating_add(line.len() as u64) > limit {
            let _ = self.prepare_rotation(path, system);
        }
        self.file.as_mut()?.write_all(line.as_bytes()).ok()
    }
    pub fn append(&mut self, path: &Path, line: &str) -> Option<()> {
        self.append_with_limit(path, line, true, 1024 * 1024)
    }
}
pub fn debug_output(line: &str) {
    let wide: Vec<_> = line.encode_utf16().chain(Some(0)).collect();
    // SAFETY: NUL-terminated UTF-16 buffer lives through this synchronous call.
    unsafe {
        windows::Win32::System::Diagnostics::Debug::OutputDebugStringW(windows::core::PCWSTR(
            wide.as_ptr(),
        ))
    };
}

#[cfg(test)]
mod tests {
    use super::*;
    fn test_path(name: &str) -> PathBuf {
        let root = std::env::temp_dir().join(format!(
            "s2u-log-{}-{name}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir(&root).unwrap();
        root.join("test.log")
    }
    #[test]
    fn child_writer() {
        let Some(path) = std::env::var_os("S2U_LOG_WRITER_PATH") else {
            return;
        };
        assert!(
            Writer::default()
                .append_with_limit(Path::new(&path), "child\n", false, 1024)
                .is_some()
        );
    }
    #[test]
    fn multiple_processes_share_append_handles() {
        let path = test_path("process");
        let mut parent = Writer::default();
        assert!(
            parent
                .append_with_limit(&path, "parent\n", false, 1024)
                .is_some()
        );
        let status = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "log::platform::tests::child_writer",
                "--nocapture",
            ])
            .env("S2U_LOG_WRITER_PATH", &path)
            .status()
            .unwrap();
        assert!(status.success());
        assert!(
            parent
                .append_with_limit(&path, "after\n", false, 1024)
                .is_some()
        );
        assert_eq!(
            std::fs::read_to_string(&path).unwrap(),
            "parent\nchild\nafter\n"
        );
        drop(parent);
        std::fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }
    #[test]
    fn stale_writer_joins_rotated_path() {
        let path = test_path("rotation");
        let mut first = Writer::default();
        let mut second = Writer::default();
        assert!(
            first
                .append_with_limit(&path, "first\n", false, 20)
                .is_some()
        );
        assert!(
            second
                .append_with_limit(&path, "second\n", false, 20)
                .is_some()
        );
        assert!(
            first
                .append_with_limit(&path, "third-long-line\n", false, 20)
                .is_some()
        );
        assert!(
            second
                .append_with_limit(&path, "rejoined\n", false, 20)
                .is_some()
        );
        assert_eq!(
            std::fs::read_to_string(&path).unwrap(),
            "third-long-line\nrejoined\n"
        );
        assert_eq!(
            std::fs::read_to_string(path.with_extension("log.1")).unwrap(),
            "first\nsecond\n"
        );
        drop((first, second));
        std::fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }
    #[test]
    fn failed_rotation_retains_line_and_retries() {
        let path = test_path("blocked");
        let archive = path.with_extension("log.1");
        let mut writer = Writer::default();
        assert!(
            writer
                .append_with_limit(&path, "123456\n", false, 10)
                .is_some()
        );
        std::fs::write(&archive, "archive\n").unwrap();
        let blocker = OpenOptions::new()
            .read(true)
            .share_mode(FILE_SHARE_READ.0 | FILE_SHARE_WRITE.0)
            .open(&archive)
            .unwrap();
        assert!(
            writer
                .append_with_limit(&path, "abcdef\n", false, 10)
                .is_some()
        );
        assert_eq!(std::fs::read_to_string(&path).unwrap(), "123456\nabcdef\n");
        drop(blocker);
        assert!(
            writer
                .append_with_limit(&path, "ghijkl\n", false, 10)
                .is_some()
        );
        assert_eq!(std::fs::read_to_string(&path).unwrap(), "ghijkl\n");
        assert_eq!(
            std::fs::read_to_string(&archive).unwrap(),
            "123456\nabcdef\n"
        );
        drop(writer);
        std::fs::remove_dir_all(path.parent().unwrap()).unwrap();
    }
}
