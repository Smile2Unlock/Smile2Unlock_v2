mod auth;
mod config;
mod embedding;
mod pipeline;
mod profile;

use std::ffi::c_char;
use std::{ptr, slice};

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
    InvalidArgument = 7,
    BufferTooSmall = 8,
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

pub use pipeline::SuFaceAuthDecision;

fn write_string_to_buffer(
    value: &str,
    out_buffer: *mut u8,
    buffer_len: usize,
    out_required_len: *mut usize,
) -> SuStatus {
    if out_required_len.is_null() {
        return SuStatus::NullArgument;
    }

    let required_len = value.len() + 1;
    unsafe {
        out_required_len.write(required_len);
    }

    if out_buffer.is_null() || buffer_len < required_len {
        return SuStatus::BufferTooSmall;
    }

    unsafe {
        let buffer = slice::from_raw_parts_mut(out_buffer, buffer_len);
        buffer[..value.len()].copy_from_slice(value.as_bytes());
        buffer[value.len()] = 0;
    }
    SuStatus::Ok
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
        ptr::write(
            out_threshold,
            config::default_config_ffi().recognition_threshold,
        );
    }
    SuStatus::Ok
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_default_config() -> SuCoreConfig {
    config::default_config_ffi()
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_load_config(
    path: *const c_char,
    out_config: *mut SuCoreConfig,
) -> SuStatus {
    config::load_config_ffi(path, out_config)
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_save_config(
    path: *const c_char,
    config: *const SuCoreConfig,
) -> SuStatus {
    config::save_config_ffi(path, config)
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_enroll_face_profile(
    store_path: *const c_char,
    label: *const c_char,
    sample_seed: *const c_char,
) -> SuStatus {
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    let label = match profile::string_from_ptr(label) {
        Ok(value) => value,
        Err(status) => return status,
    };
    let sample_seed = match profile::string_from_ptr(sample_seed) {
        Ok(value) => value,
        Err(status) => return status,
    };

    match profile::enroll_profile(&store_path, &label, &sample_seed) {
        Ok(_) => SuStatus::Ok,
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_delete_face_profile(
    store_path: *const c_char,
    profile_id: *const c_char,
    out_deleted: *mut bool,
) -> SuStatus {
    if out_deleted.is_null() {
        return SuStatus::NullArgument;
    }

    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    let profile_id = match profile::string_from_ptr(profile_id) {
        Ok(value) => value,
        Err(status) => return status,
    };

    match profile::delete_profile(&store_path, &profile_id) {
        Ok(deleted) => {
            unsafe {
                out_deleted.write(deleted);
            }
            SuStatus::Ok
        }
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_list_face_profiles_json(
    store_path: *const c_char,
    out_buffer: *mut u8,
    buffer_len: usize,
    out_required_len: *mut usize,
) -> SuStatus {
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };

    match profile::list_profiles_json(&store_path) {
        Ok(json) => write_string_to_buffer(&json, out_buffer, buffer_len, out_required_len),
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_authenticate_face_sample(
    store_path: *const c_char,
    sample_seed: *const c_char,
    threshold: f32,
) -> SuFaceAuthDecision {
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => {
            return SuFaceAuthDecision {
                status,
                accepted: false,
                score: 0.0,
                profile_count: 0,
            };
        }
    };
    let sample_seed = match profile::string_from_ptr(sample_seed) {
        Ok(value) => value,
        Err(status) => {
            return SuFaceAuthDecision {
                status,
                accepted: false,
                score: 0.0,
                profile_count: 0,
            };
        }
    };

    pipeline::authenticate_sample_ffi(&store_path, &sample_seed, threshold)
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_authenticate_face_sample_report_json(
    store_path: *const c_char,
    sample_seed: *const c_char,
    threshold: f32,
    out_buffer: *mut u8,
    buffer_len: usize,
    out_required_len: *mut usize,
) -> SuStatus {
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    let sample_seed = match profile::string_from_ptr(sample_seed) {
        Ok(value) => value,
        Err(status) => return status,
    };

    match pipeline::authenticate_sample_report_json(&store_path, &sample_seed, threshold) {
        Ok(json) => write_string_to_buffer(&json, out_buffer, buffer_len, out_required_len),
        Err(status) => status,
    }
}
