//! C ABI surface for the Smile2Unlock core.
//!
//! All `extern "C"` entry points and the FFI-side struct definitions live here
//! so that `lib.rs` stays a small module manifest. Domain logic (auth, config,
//! embedding, pipeline, profile) is owned by its own modules and only reached
//! through these narrow entry points.

use std::ffi::c_char;
use std::{ptr, slice};

use crate::auth;
use crate::config;
use crate::pipeline;
use crate::profile;
use crate::protocol;
use crate::{SuAuthDecision, SuControlRequest, SuCoreConfig, SuFaceProfileSummary, SuStatus};

use profile::{PROFILE_ID_CAP, PROFILE_LABEL_CAP};

// Re-export the FFI structs that `su_core.h` promises to C++. They are defined
// in the domain modules (pipeline/auth) and the crate root; keeping them there
// avoids a second source of truth for their layout.
pub use crate::pipeline::{SuFaceAuthDecision, SuFaceAuthReport};

#[unsafe(no_mangle)]
pub extern "C" fn su_core_parse_control_request(
    input: *const u8,
    input_len: usize,
    out_request: *mut SuControlRequest,
) -> SuStatus {
    protocol::parse_control_request_ffi(input, input_len, out_request)
}

pub(crate) fn write_string_to_buffer(
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
    face_sample_source: *const c_char,
) -> SuStatus {
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    let label = match profile::string_from_ptr(label) {
        Ok(value) => value,
        Err(status) => return status,
    };
    let face_sample_source = match profile::string_from_ptr(face_sample_source) {
        Ok(value) => value,
        Err(status) => return status,
    };

    match profile::enroll_profile(&store_path, &label, &face_sample_source) {
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
pub extern "C" fn su_core_list_face_profile_summaries(
    store_path: *const c_char,
    out_profiles: *mut SuFaceProfileSummary,
    profile_capacity: usize,
    out_profile_count: *mut usize,
) -> SuStatus {
    if out_profile_count.is_null() {
        return SuStatus::NullArgument;
    }

    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    let profiles = match profile::list_profiles(&store_path) {
        Ok(profiles) => profiles,
        Err(status) => return status,
    };

    unsafe {
        out_profile_count.write(profiles.len());
    }
    if profiles.is_empty() {
        return SuStatus::Ok;
    }
    if out_profiles.is_null() || profile_capacity < profiles.len() {
        return SuStatus::BufferTooSmall;
    }

    let out = unsafe { slice::from_raw_parts_mut(out_profiles, profile_capacity) };
    for (index, profile) in profiles.iter().enumerate() {
        let mut id = [0; PROFILE_ID_CAP];
        let mut label = [0; PROFILE_LABEL_CAP];
        profile::copy_str_to_fixed(&profile.id, &mut id);
        profile::copy_str_to_fixed(&profile.label, &mut label);
        out[index] = SuFaceProfileSummary {
            id,
            label,
            created_at_unix: profile.created_at_unix,
        };
    }

    SuStatus::Ok
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_authenticate_face_sample(
    store_path: *const c_char,
    face_sample_source: *const c_char,
    threshold: f32,
) -> SuFaceAuthDecision {
    su_core_authenticate_face_sample_with_liveness(store_path, face_sample_source, threshold, true)
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_authenticate_face_sample_with_liveness(
    store_path: *const c_char,
    face_sample_source: *const c_char,
    threshold: f32,
    liveness_ok: bool,
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
    let face_sample_source = match profile::string_from_ptr(face_sample_source) {
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

    pipeline::authenticate_sample_with_liveness_ffi(
        &store_path,
        &face_sample_source,
        threshold,
        liveness_ok,
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_authenticate_face_sample_report_json(
    store_path: *const c_char,
    face_sample_source: *const c_char,
    threshold: f32,
    out_buffer: *mut u8,
    buffer_len: usize,
    out_required_len: *mut usize,
) -> SuStatus {
    su_core_authenticate_face_sample_report_json_with_liveness(
        store_path,
        face_sample_source,
        threshold,
        true,
        out_buffer,
        buffer_len,
        out_required_len,
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_authenticate_face_sample_report_json_with_liveness(
    store_path: *const c_char,
    face_sample_source: *const c_char,
    threshold: f32,
    liveness_ok: bool,
    out_buffer: *mut u8,
    buffer_len: usize,
    out_required_len: *mut usize,
) -> SuStatus {
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    let face_sample_source = match profile::string_from_ptr(face_sample_source) {
        Ok(value) => value,
        Err(status) => return status,
    };

    match pipeline::authenticate_sample_report_json_with_liveness(
        &store_path,
        &face_sample_source,
        threshold,
        liveness_ok,
    ) {
        Ok(json) => write_string_to_buffer(&json, out_buffer, buffer_len, out_required_len),
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_authenticate_face_sample_report(
    store_path: *const c_char,
    face_sample_source: *const c_char,
    threshold: f32,
) -> SuFaceAuthReport {
    su_core_authenticate_face_sample_report_with_liveness(
        store_path,
        face_sample_source,
        threshold,
        true,
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_authenticate_face_sample_report_with_liveness(
    store_path: *const c_char,
    face_sample_source: *const c_char,
    threshold: f32,
    liveness_ok: bool,
) -> SuFaceAuthReport {
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => {
            return pipeline::FaceAuthReport {
                accepted: false,
                score: 0.0,
                threshold,
                liveness_ok,
                profile_count: 0,
                best_profile_id: None,
                best_profile_label: None,
                reason: "invalid profile store path".to_owned(),
            }
            .to_ffi(status);
        }
    };
    let face_sample_source = match profile::string_from_ptr(face_sample_source) {
        Ok(value) => value,
        Err(status) => {
            return pipeline::FaceAuthReport {
                accepted: false,
                score: 0.0,
                threshold,
                liveness_ok,
                profile_count: 0,
                best_profile_id: None,
                best_profile_label: None,
                reason: "invalid face sample".to_owned(),
            }
            .to_ffi(status);
        }
    };

    pipeline::authenticate_sample_report_with_liveness_ffi(
        &store_path,
        &face_sample_source,
        threshold,
        liveness_ok,
    )
}
