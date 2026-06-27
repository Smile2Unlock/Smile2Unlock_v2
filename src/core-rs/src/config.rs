use std::ffi::{CStr, c_char};
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::{SuCoreConfig, SuStatus};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppConfig {
    pub version: u32,
    pub selected_camera: i32,
    pub recognition_threshold: f32,
    pub liveness_detection: bool,
    pub preview_fps: u32,
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            version: 1,
            selected_camera: 0,
            recognition_threshold: 0.65,
            liveness_detection: true,
            preview_fps: 15,
        }
    }
}

impl From<AppConfig> for SuCoreConfig {
    fn from(value: AppConfig) -> Self {
        Self {
            version: value.version,
            selected_camera: value.selected_camera,
            recognition_threshold: value.recognition_threshold,
            liveness_detection: value.liveness_detection,
            preview_fps: value.preview_fps,
        }
    }
}

impl From<SuCoreConfig> for AppConfig {
    fn from(value: SuCoreConfig) -> Self {
        Self {
            version: if value.version == 0 { 1 } else { value.version },
            selected_camera: value.selected_camera,
            recognition_threshold: value.recognition_threshold,
            liveness_detection: value.liveness_detection,
            preview_fps: if value.preview_fps == 0 {
                15
            } else {
                value.preview_fps
            },
        }
    }
}

fn path_from_ptr(path: *const c_char) -> Result<PathBuf, SuStatus> {
    if path.is_null() {
        return Err(SuStatus::NullArgument);
    }

    let raw = unsafe { CStr::from_ptr(path) };
    raw.to_str()
        .map(PathBuf::from)
        .map_err(|_| SuStatus::InvalidUtf8)
}

pub(crate) fn load_config(path: &Path) -> Result<AppConfig, SuStatus> {
    if !path.exists() {
        return Ok(AppConfig::default());
    }

    let text = fs::read_to_string(path).map_err(|_| SuStatus::IoError)?;
    toml::from_str::<AppConfig>(&text).map_err(|_| SuStatus::ParseError)
}

fn save_config(path: &Path, config: &AppConfig) -> Result<(), SuStatus> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|_| SuStatus::IoError)?;
    }

    let text = toml::to_string_pretty(config).map_err(|_| SuStatus::WriteError)?;
    let tmp_path = path.with_extension("tmp");
    fs::write(&tmp_path, text).map_err(|_| SuStatus::WriteError)?;
    fs::rename(&tmp_path, path).map_err(|_| SuStatus::WriteError)?;
    Ok(())
}

pub fn default_config_ffi() -> SuCoreConfig {
    AppConfig::default().into()
}

pub fn load_config_ffi(path: *const c_char, out_config: *mut SuCoreConfig) -> SuStatus {
    if out_config.is_null() {
        return SuStatus::NullArgument;
    }

    let path = match path_from_ptr(path) {
        Ok(path) => path,
        Err(status) => return status,
    };

    match load_config(&path) {
        Ok(config) => {
            unsafe {
                out_config.write(config.into());
            }
            SuStatus::Ok
        }
        Err(status) => status,
    }
}

pub fn save_config_ffi(path: *const c_char, config: *const SuCoreConfig) -> SuStatus {
    if config.is_null() {
        return SuStatus::NullArgument;
    }

    let path = match path_from_ptr(path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    let config = AppConfig::from(unsafe { *config });

    match save_config(&path, &config) {
        Ok(()) => SuStatus::Ok,
        Err(status) => status,
    }
}
