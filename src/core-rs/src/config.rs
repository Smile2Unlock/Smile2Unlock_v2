use std::ffi::{CStr, c_char};
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::storage::atomic_write_private;
use crate::{SuCoreConfig, SuStatus};

const MAX_PREVIEW_FPS: u32 = 120;

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
    #[serde(default = "default_recognition_mode")]
    pub recognition_mode: u32,
    #[serde(default = "default_auto_delay_sec")]
    pub auto_delay_sec: u32,
    #[serde(default = "default_retry_delay_sec")]
    pub retry_delay_sec: u32,
    #[serde(default = "default_timeout_sec")]
    pub timeout_sec: u32,
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
fn default_recognition_mode() -> u32 {
    0 // manual
}
fn default_auto_delay_sec() -> u32 {
    3
}
fn default_retry_delay_sec() -> u32 {
    5
}
fn default_timeout_sec() -> u32 {
    30
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
            recognition_mode: default_recognition_mode(),
            auto_delay_sec: default_auto_delay_sec(),
            retry_delay_sec: default_retry_delay_sec(),
            timeout_sec: default_timeout_sec(),
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
            recognition_mode: value.recognition_mode,
            auto_delay_sec: value.auto_delay_sec,
            retry_delay_sec: value.retry_delay_sec,
            timeout_sec: value.timeout_sec,
        }
    }
}

impl From<SuCoreConfig> for AppConfig {
    fn from(value: SuCoreConfig) -> Self {
        normalize_config(Self {
            version: if value.version == 0 {
                default_version()
            } else {
                value.version
            },
            selected_camera: value.selected_camera,
            recognition_threshold: value.recognition_threshold,
            liveness_detection: value.liveness_detection,
            liveness_threshold: value.liveness_threshold,
            preview_fps: value.preview_fps,
            recognition_mode: value.recognition_mode,
            auto_delay_sec: value.auto_delay_sec,
            retry_delay_sec: value.retry_delay_sec,
            timeout_sec: value.timeout_sec,
        })
    }
}

fn valid_threshold(value: f32) -> bool {
    value.is_finite() && value > 0.0 && value <= 1.0
}

fn normalize_config(mut config: AppConfig) -> AppConfig {
    if config.version == 0 {
        config.version = default_version();
    }
    if config.selected_camera < 0 {
        config.selected_camera = 0;
    }
    if !valid_threshold(config.recognition_threshold) {
        config.recognition_threshold = default_recognition_threshold();
    }
    if !valid_threshold(config.liveness_threshold) {
        config.liveness_threshold = default_liveness_threshold();
    }
    if config.preview_fps == 0 || config.preview_fps > MAX_PREVIEW_FPS {
        config.preview_fps = default_preview_fps();
    }
    if config.recognition_mode > 1 {
        config.recognition_mode = default_recognition_mode();
    }
    if config.auto_delay_sec > 3600 {
        config.auto_delay_sec = default_auto_delay_sec();
    }
    if config.retry_delay_sec == 0 || config.retry_delay_sec > 3600 {
        config.retry_delay_sec = default_retry_delay_sec();
    }
    if config.timeout_sec < 5 || config.timeout_sec > 600 {
        config.timeout_sec = default_timeout_sec();
    }
    config
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
        Ok(config) => Ok(normalize_config(config)),
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
    let text = toml::to_string_pretty(&normalize_config(config.clone()))
        .map_err(|_| SuStatus::WriteError)?;
    atomic_write_private(path, text.as_bytes())
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
