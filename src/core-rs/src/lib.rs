use std::ffi::{c_char, CStr};
use std::ptr;

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SuStatus {
    Ok = 0,
    NullArgument = 1,
    InvalidUtf8 = 2,
    UserDenied = 3,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct SuAuthDecision {
    pub status: SuStatus,
    pub accepted: bool,
}

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

#[unsafe(no_mangle)]
pub extern "C" fn su_core_default_threshold(out_threshold: *mut f32) -> SuStatus {
    if out_threshold.is_null() {
        return SuStatus::NullArgument;
    }

    unsafe {
        ptr::write(out_threshold, 0.65);
    }
    SuStatus::Ok
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    #[test]
    fn accepts_valid_user_above_threshold() {
        let name = CString::new("alice").unwrap();
        let decision = su_core_evaluate_auth(name.as_ptr(), 0.72, 0.65, true);

        assert_eq!(decision.status, SuStatus::Ok);
        assert!(decision.accepted);
    }

    #[test]
    fn rejects_without_liveness() {
        let name = CString::new("alice").unwrap();
        let decision = su_core_evaluate_auth(name.as_ptr(), 0.72, 0.65, false);

        assert_eq!(decision.status, SuStatus::Ok);
        assert!(!decision.accepted);
    }
}
