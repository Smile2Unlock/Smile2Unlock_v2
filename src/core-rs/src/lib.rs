mod auth;
mod config;

use std::ffi::c_char;
use std::ptr;

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SuStatus {
    Ok = 0,
    NullArgument = 1,
    InvalidUtf8 = 2,
    UserDenied = 3,
    IoError = 4,
    ParseError = 5,
    WriteError = 6,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct SuAuthDecision {
    pub status: SuStatus,
    pub accepted: bool,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct SuCoreConfig {
    pub version: u32,
    pub selected_camera: i32,
    pub recognition_threshold: f32,
    pub liveness_detection: bool,
    pub preview_fps: u32,
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_version_major() -> u32 {
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_evaluate_auth(
    username: *const c_char,
    similarity: f32,
    threshold: f32,
    liveness_ok: bool,
) -> SuAuthDecision {
    auth::evaluate_auth_ffi(username, similarity, threshold, liveness_ok)
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_default_threshold(out_threshold: *mut f32) -> SuStatus {
    if out_threshold.is_null() {
        return SuStatus::NullArgument;
    }

    unsafe {
        ptr::write(out_threshold, config::default_config_ffi().recognition_threshold);
    }
    SuStatus::Ok
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_default_config() -> SuCoreConfig {
    config::default_config_ffi()
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_load_config(path: *const c_char, out_config: *mut SuCoreConfig) -> SuStatus {
    config::load_config_ffi(path, out_config)
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_save_config(path: *const c_char, config: *const SuCoreConfig) -> SuStatus {
    config::save_config_ffi(path, config)
}
