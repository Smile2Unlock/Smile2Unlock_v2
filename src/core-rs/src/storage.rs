use std::ffi::OsStr;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

#[cfg(windows)]
fn replace_file(source: &Path, destination: &Path) -> Result<(), SuStatus> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Storage::FileSystem::{
        MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH, MoveFileExW,
    };

    let source = source
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect::<Vec<_>>();
    let destination = destination
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect::<Vec<_>>();
    let moved = unsafe {
        MoveFileExW(
            source.as_ptr(),
            destination.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if moved == 0 {
        return Err(SuStatus::WriteError);
    }
    Ok(())
}

use crate::SuStatus;

static TEMP_COUNTER: AtomicU64 = AtomicU64::new(0);

fn create_parent(parent: &Path) -> Result<(), SuStatus> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::DirBuilderExt;

        let mut builder = fs::DirBuilder::new();
        builder.recursive(true).mode(0o700);
        builder.create(parent).map_err(|_| SuStatus::IoError)
    }
    #[cfg(not(unix))]
    {
        fs::create_dir_all(parent).map_err(|_| SuStatus::IoError)
    }
}

fn unique_temp_path(path: &Path) -> PathBuf {
    let sequence = TEMP_COUNTER.fetch_add(1, Ordering::Relaxed);
    let file_name = path.file_name().unwrap_or_else(|| OsStr::new("data"));
    path.with_file_name(format!(
        ".{}.tmp-{}-{sequence}",
        file_name.to_string_lossy(),
        std::process::id(),
    ))
}

pub(crate) fn atomic_write_private(path: &Path, contents: &[u8]) -> Result<(), SuStatus> {
    if let Some(parent) = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        create_parent(parent)?;
    }

    let temp_path = unique_temp_path(path);
    let mut options = OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }

    let write_result = (|| {
        let mut file = options.open(&temp_path).map_err(|_| SuStatus::WriteError)?;
        file.write_all(contents).map_err(|_| SuStatus::WriteError)?;
        file.sync_all().map_err(|_| SuStatus::WriteError)?;

        #[cfg(windows)]
        replace_file(&temp_path, path)?;
        #[cfg(not(windows))]
        fs::rename(&temp_path, path).map_err(|_| SuStatus::WriteError)?;

        #[cfg(unix)]
        if let Some(parent) = path
            .parent()
            .filter(|parent| !parent.as_os_str().is_empty())
        {
            fs::File::open(parent)
                .and_then(|directory| directory.sync_all())
                .map_err(|_| SuStatus::WriteError)?;
        }
        Ok(())
    })();

    if write_result.is_err() {
        let _ = fs::remove_file(&temp_path);
    }
    write_result
}
