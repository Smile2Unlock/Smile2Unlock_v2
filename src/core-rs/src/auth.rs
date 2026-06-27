use std::ffi::{CStr, c_char};

use crate::{SuAuthDecision, SuStatus};

fn username_from_ptr(username: *const c_char) -> Result<String, SuStatus> {
    if username.is_null() {
        return Err(SuStatus::NullArgument);
    }

    let raw = unsafe { CStr::from_ptr(username) };
    match raw.to_str() {
        Ok(name) => Ok(name.trim().to_owned()),
        Err(_) => Err(SuStatus::InvalidUtf8),
    }
}

fn evaluate_auth(username: &str, similarity: f32, threshold: f32, liveness_ok: bool) -> bool {
    !username.is_empty() && liveness_ok && similarity >= threshold
}

pub fn evaluate_auth_ffi(
    username: *const c_char,
    similarity: f32,
    threshold: f32,
    liveness_ok: bool,
) -> SuAuthDecision {
    match username_from_ptr(username) {
        Ok(name) => SuAuthDecision {
            status: SuStatus::Ok,
            accepted: evaluate_auth(&name, similarity, threshold, liveness_ok),
        },
        Err(status) => SuAuthDecision {
            status,
            accepted: false,
        },
    }
}
