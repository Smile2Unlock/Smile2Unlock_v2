use std::ffi::{CStr, c_char};
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::{SuCoreConfig, SuStatus};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppConfig {
    #[serde(default = "default_version")]
    pub version: u32,
    #[serde(default)]
    pub selected_camera: i32,
    #[serde(default = "default_recognition_threshold")]
    pub recognition_threshold: f32,
    #[serde(default = "default_liveness_detection")]
    pub liveness_detection: bool,
    #[serde(default = "default_liveness_threshold")]
    pub liveness_threshold: f32,
    #[serde(default = "default_preview_fps")]
    pub preview_fps: u32,
}

fn default_version() -> u32 {
    1
}
fn default_recognition_threshold() -> f32 {
    0.65
}
fn default_liveness_detection() -> bool {
    true
}
fn default_liveness_threshold() -> f32 {
    0.50
}
fn default_preview_fps() -> u32 {
    15
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            version: default_version(),
            selected_camera: 0,
            recognition_threshold: default_recognition_threshold(),
            liveness_detection: default_liveness_detection(),
            liveness_threshold: default_liveness_threshold(),
            preview_fps: default_preview_fps(),
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
            liveness_threshold: value.liveness_threshold,
            preview_fps: value.preview_fps,
        }
    }
}

impl From<SuCoreConfig> for AppConfig {
    fn from(value: SuCoreConfig) -> Self {
        Self {
            version: if value.version == 0 { default_version() } else { value.version },
            selected_camera: value.selected_camera,
            recognition_threshold: if value.recognition_threshold <= 0.0 {
                default_recognition_threshold()
            } else {
                value.recognition_threshold
            },
            liveness_detection: value.liveness_detection,
            liveness_threshold: if value.liveness_threshold <= 0.0 {
                default_liveness_threshold()
            } else {
                value.liveness_threshold
            },
            preview_fps: if value.preview_fps == 0 {
                default_preview_fps()
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
    // A corrupt or partially-valid config falls back to defaults instead of
    // propagating a parse error, so the app stays usable. Missing fields are
    // already tolerated via #[serde(default)] on each field; a fully malformed
    // document is logged and replaced wholesale.
    match toml::from_str::<AppConfig>(&text) {
        Ok(config) => Ok(config),
        Err(error) => {
            eprintln!(
                "smile2unlock: config at {} failed to parse ({}); using defaults",
                path.display(),
                error
            );
            Ok(AppConfig::default())
        }
    }
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
