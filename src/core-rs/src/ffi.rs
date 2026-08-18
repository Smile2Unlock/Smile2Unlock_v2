//! C ABI surface for the Smile2Unlock core.
//!
//! All `extern "C"` entry points and the FFI-side struct definitions live here
//! so that `lib.rs` stays a small module manifest. Domain logic (auth, config,
//! embedding, pipeline, profile) is owned by its own modules and only reached
//! through these narrow entry points.

use std::ffi::{CStr, c_char};
use std::{ptr, slice};

use crate::auth;
use crate::config;
use crate::encrypted_store::AccountId;
use crate::password;
use crate::pipeline;
use crate::profile;
use crate::protocol;
use crate::{
    SuAccountKind, SuAuthDecision, SuControlRequest, SuCoreConfig, SuEncryptedStoreContext,
    SuFaceProfileSummary, SuStatus, SuWindowsAccountKind,
};

use profile::{PROFILE_ID_CAP, PROFILE_LABEL_CAP};

fn encrypted_context_from_ptr<'a>(
    context: *const SuEncryptedStoreContext,
) -> Result<profile::EncryptedProfileContext<'a>, SuStatus> {
    if context.is_null() {
        return Err(SuStatus::NullArgument);
    }
    let context = unsafe { &*context };
    if context.master_key.is_null() || context.master_key_len != 32 || context.key_version == 0 {
        return Err(SuStatus::KeyUnavailable);
    }
    let master_key_slice = unsafe { slice::from_raw_parts(context.master_key, 32) };
    let master_key: &[u8; 32] = master_key_slice
        .try_into()
        .map_err(|_| SuStatus::KeyUnavailable)?;
    let account = match context.account_kind {
        value if value == SuAccountKind::LinuxUid as u32 => AccountId::LinuxUid(context.linux_uid),
        value if value == SuAccountKind::WindowsSid as u32 => {
            if context.windows_sid.is_null() {
                return Err(SuStatus::InvalidArgument);
            }
            let sid = unsafe { CStr::from_ptr(context.windows_sid) }
                .to_str()
                .map_err(|_| SuStatus::InvalidUtf8)?;
            if sid.len() < 5
                || sid.len() > 184
                || !sid.starts_with("S-1-")
                || !sid
                    .bytes()
                    .all(|byte| byte.is_ascii_digit() || byte == b'-' || byte == b'S')
            {
                return Err(SuStatus::InvalidArgument);
            }
            AccountId::WindowsSid(sid.to_owned())
        }
        _ => return Err(SuStatus::InvalidArgument),
    };
    Ok(profile::EncryptedProfileContext {
        master_key,
        key_version: context.key_version,
        account,
    })
}

fn write_profile_summaries(
    profiles: &[profile::FaceProfile],
    out_profiles: *mut SuFaceProfileSummary,
    profile_capacity: usize,
    out_profile_count: *mut usize,
) -> SuStatus {
    if out_profile_count.is_null() {
        return SuStatus::NullArgument;
    }
    unsafe { out_profile_count.write(profiles.len()) };
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

fn failed_auth_report(
    status: SuStatus,
    threshold: f32,
    liveness_ok: bool,
    reason: &str,
) -> SuFaceAuthReport {
    pipeline::FaceAuthReport {
        accepted: false,
        score: 0.0,
        threshold,
        liveness_ok,
        profile_count: 0,
        best_profile_id: None,
        best_profile_label: None,
        reason: reason.to_owned(),
    }
    .to_ffi(status)
}

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
    // Core ABI major, kept independent from the product's patch/minor
    // version. The current rewrite exports ABI v2.
    2
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

    write_profile_summaries(&profiles, out_profiles, profile_capacity, out_profile_count)
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_encrypted_enroll_face_profile(
    context: *const SuEncryptedStoreContext,
    store_path: *const c_char,
    label: *const c_char,
    face_sample_source: *const c_char,
) -> SuStatus {
    let context = match encrypted_context_from_ptr(context) {
        Ok(context) => context,
        Err(status) => return status,
    };
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    let label = match profile::string_from_ptr(label) {
        Ok(label) => label,
        Err(status) => return status,
    };
    let face_sample_source = match profile::string_from_ptr(face_sample_source) {
        Ok(source) => source,
        Err(status) => return status,
    };
    profile::enroll_profile_encrypted(&store_path, &context, &label, &face_sample_source)
        .map(|_| SuStatus::Ok)
        .unwrap_or_else(|status| status)
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_encrypted_delete_face_profile(
    context: *const SuEncryptedStoreContext,
    store_path: *const c_char,
    profile_id: *const c_char,
    out_deleted: *mut bool,
) -> SuStatus {
    if out_deleted.is_null() {
        return SuStatus::NullArgument;
    }
    let context = match encrypted_context_from_ptr(context) {
        Ok(context) => context,
        Err(status) => return status,
    };
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    let profile_id = match profile::string_from_ptr(profile_id) {
        Ok(profile_id) => profile_id,
        Err(status) => return status,
    };
    match profile::delete_profile_encrypted(&store_path, &context, &profile_id) {
        Ok(deleted) => {
            unsafe { out_deleted.write(deleted) };
            SuStatus::Ok
        }
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_encrypted_list_face_profiles_json(
    context: *const SuEncryptedStoreContext,
    store_path: *const c_char,
    out_buffer: *mut u8,
    buffer_len: usize,
    out_required_len: *mut usize,
) -> SuStatus {
    let context = match encrypted_context_from_ptr(context) {
        Ok(context) => context,
        Err(status) => return status,
    };
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    match profile::list_encrypted_profiles_json(&store_path, &context) {
        Ok(json) => write_string_to_buffer(&json, out_buffer, buffer_len, out_required_len),
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_encrypted_list_face_profile_summaries(
    context: *const SuEncryptedStoreContext,
    store_path: *const c_char,
    out_profiles: *mut SuFaceProfileSummary,
    profile_capacity: usize,
    out_profile_count: *mut usize,
) -> SuStatus {
    let context = match encrypted_context_from_ptr(context) {
        Ok(context) => context,
        Err(status) => return status,
    };
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    match profile::load_encrypted_store(&store_path, &context) {
        Ok(store) => write_profile_summaries(
            &store.profiles,
            out_profiles,
            profile_capacity,
            out_profile_count,
        ),
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_encrypted_authenticate_face_sample_report(
    context: *const SuEncryptedStoreContext,
    store_path: *const c_char,
    face_sample_source: *const c_char,
    threshold: f32,
    liveness_ok: bool,
) -> SuFaceAuthReport {
    let context = match encrypted_context_from_ptr(context) {
        Ok(context) => context,
        Err(status) => {
            return failed_auth_report(status, threshold, liveness_ok, "invalid key context");
        }
    };
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => {
            return failed_auth_report(status, threshold, liveness_ok, "invalid store path");
        }
    };
    let face_sample_source = match profile::string_from_ptr(face_sample_source) {
        Ok(source) => source,
        Err(status) => {
            return failed_auth_report(status, threshold, liveness_ok, "invalid face sample");
        }
    };
    match pipeline::authenticate_encrypted_sample_with_liveness(
        &store_path,
        &context,
        &face_sample_source,
        threshold,
        liveness_ok,
    ) {
        Ok(report) => report.to_ffi(SuStatus::Ok),
        Err(status) => {
            failed_auth_report(status, threshold, liveness_ok, "face authentication failed")
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_migrate_plaintext_face_profiles(
    context: *const SuEncryptedStoreContext,
    legacy_path: *const c_char,
    encrypted_path: *const c_char,
    out_migrated: *mut bool,
) -> SuStatus {
    if out_migrated.is_null() {
        return SuStatus::NullArgument;
    }
    let context = match encrypted_context_from_ptr(context) {
        Ok(context) => context,
        Err(status) => return status,
    };
    let legacy_path = match profile::path_from_ptr(legacy_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    let encrypted_path = match profile::path_from_ptr(encrypted_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    match profile::migrate_plaintext_store(&legacy_path, &encrypted_path, &context, false) {
        Ok(migrated) => {
            unsafe { out_migrated.write(migrated) };
            SuStatus::Ok
        }
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_store_windows_logon_secret(
    context: *const SuEncryptedStoreContext,
    store_path: *const c_char,
    account_kind: u32,
    canonical_username: *const c_char,
    password: *const u16,
    password_len: usize,
    out_generation: *mut u64,
) -> SuStatus {
    if password.is_null() || out_generation.is_null() {
        return SuStatus::NullArgument;
    }
    let context = match encrypted_context_from_ptr(context) {
        Ok(context) => context,
        Err(status) => return status,
    };
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    let canonical_username = match profile::string_from_ptr(canonical_username) {
        Ok(username) => username,
        Err(status) => return status,
    };
    if password_len == 0 || password_len > password::MAX_PASSWORD_UNITS {
        return SuStatus::InvalidArgument;
    }
    let password = unsafe { slice::from_raw_parts(password, password_len) };
    let account_kind = match account_kind {
        value if value == SuWindowsAccountKind::Local as u32 => SuWindowsAccountKind::Local,
        value if value == SuWindowsAccountKind::Microsoft as u32 => SuWindowsAccountKind::Microsoft,
        _ => return SuStatus::InvalidArgument,
    };
    match password::store_logon_secret(
        &store_path,
        &context,
        account_kind,
        &canonical_username,
        password,
    ) {
        Ok(generation) => {
            unsafe { out_generation.write(generation) };
            SuStatus::Ok
        }
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_prepare_windows_logon_secret(
    context: *const SuEncryptedStoreContext,
    store_path: *const c_char,
    request_id: u64,
    logon_session_id: u32,
    out_password: *mut u16,
    password_capacity: usize,
    out_password_len: *mut usize,
) -> SuStatus {
    if out_password.is_null() || out_password_len.is_null() {
        return SuStatus::NullArgument;
    }
    if password_capacity < password::MAX_PASSWORD_UNITS + 1 {
        unsafe { out_password_len.write(password::MAX_PASSWORD_UNITS + 1) };
        return SuStatus::BufferTooSmall;
    }
    let context = match encrypted_context_from_ptr(context) {
        Ok(context) => context,
        Err(status) => return status,
    };
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    match password::prepare_logon_secret(&store_path, &context, request_id, logon_session_id) {
        Ok(password) => {
            let output = unsafe { slice::from_raw_parts_mut(out_password, password_capacity) };
            output[..password.len()].copy_from_slice(&password);
            output[password.len()] = 0;
            unsafe { out_password_len.write(password.len()) };
            SuStatus::Ok
        }
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_mark_windows_logon_secret_stale(
    context: *const SuEncryptedStoreContext,
    store_path: *const c_char,
) -> SuStatus {
    let context = match encrypted_context_from_ptr(context) {
        Ok(context) => context,
        Err(status) => return status,
    };
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    password::mark_logon_secret_stale(&store_path, &context)
        .map(|_| SuStatus::Ok)
        .unwrap_or_else(|status| status)
}

#[unsafe(no_mangle)]
pub extern "C" fn su_core_clear_windows_logon_secret(
    context: *const SuEncryptedStoreContext,
    store_path: *const c_char,
    out_cleared: *mut bool,
) -> SuStatus {
    if out_cleared.is_null() {
        return SuStatus::NullArgument;
    }
    let context = match encrypted_context_from_ptr(context) {
        Ok(context) => context,
        Err(status) => return status,
    };
    let store_path = match profile::path_from_ptr(store_path) {
        Ok(path) => path,
        Err(status) => return status,
    };
    match password::clear_logon_secret(&store_path, &context) {
        Ok(cleared) => {
            unsafe { out_cleared.write(cleared) };
            SuStatus::Ok
        }
        Err(status) => status,
    }
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
