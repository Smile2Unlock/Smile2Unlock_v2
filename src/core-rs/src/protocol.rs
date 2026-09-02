use std::slice;

use serde::Deserialize;

use crate::SuStatus;
use crate::profile::copy_str_to_fixed;

pub const CONTROL_PROTOCOL_VERSION: u32 = 1;
pub const CONTROL_USERNAME_CAP: usize = 256;
pub const CONTROL_PROFILE_ID_CAP: usize = 64;
pub const CONTROL_PROFILE_LABEL_CAP: usize = 128;
pub const CONTROL_SAMPLE_SOURCE_CAP: usize = 48 * 1024;

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SuControlMessageType {
    Authenticate = 1,
    Status = 2,
    Cancel = 3,
    StorageStatus = 4,
    ListProfiles = 5,
    EnrollProfile = 6,
    DeleteProfile = 7,
    MigrateProfiles = 8,
    VerifyProfile = 9,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct SuControlRequest {
    pub msg_type: SuControlMessageType,
    pub request_id: u64,
    pub target_request_id: u64,
    pub username: [u8; CONTROL_USERNAME_CAP],
    pub profile_id: [u8; CONTROL_PROFILE_ID_CAP],
    pub label: [u8; CONTROL_PROFILE_LABEL_CAP],
    pub face_sample_source: [u8; CONTROL_SAMPLE_SOURCE_CAP],
    pub liveness_ok: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ControlRequest {
    Authenticate {
        request_id: u64,
        username: String,
    },
    Status {
        request_id: u64,
    },
    Cancel {
        request_id: u64,
        target_request_id: u64,
    },
    StorageStatus {
        request_id: u64,
    },
    ListProfiles {
        request_id: u64,
        username: String,
    },
    EnrollProfile {
        request_id: u64,
        username: String,
        label: String,
        face_sample_source: String,
    },
    DeleteProfile {
        request_id: u64,
        username: String,
        profile_id: String,
    },
    MigrateProfiles {
        request_id: u64,
        username: String,
    },
    VerifyProfile {
        request_id: u64,
        username: String,
        face_sample_source: String,
        liveness_ok: bool,
    },
}

#[derive(Debug, Deserialize)]
#[serde(tag = "msg_type", rename_all = "snake_case", deny_unknown_fields)]
enum ControlRequestWire {
    Authenticate {
        version: u32,
        request_id: u64,
        username: String,
    },
    Status {
        version: u32,
        request_id: u64,
    },
    Cancel {
        version: u32,
        request_id: u64,
        target_request_id: u64,
    },
    StorageStatus {
        version: u32,
        request_id: u64,
    },
    ListProfiles {
        version: u32,
        request_id: u64,
        username: String,
    },
    EnrollProfile {
        version: u32,
        request_id: u64,
        username: String,
        label: String,
        face_sample_source: String,
    },
    DeleteProfile {
        version: u32,
        request_id: u64,
        username: String,
        profile_id: String,
    },
    MigrateProfiles {
        version: u32,
        request_id: u64,
        username: String,
    },
    VerifyProfile {
        version: u32,
        request_id: u64,
        username: String,
        face_sample_source: String,
        liveness_ok: bool,
    },
}

fn valid_username(username: &str) -> bool {
    !username.is_empty()
        && username.len() < CONTROL_USERNAME_CAP
        && !username.chars().any(char::is_control)
}

fn valid_text(value: &str, capacity: usize) -> bool {
    !value.is_empty() && value.len() < capacity && !value.chars().any(char::is_control)
}

fn valid_sample_source(value: &str) -> bool {
    !value.is_empty()
        && value.len() < CONTROL_SAMPLE_SOURCE_CAP
        && !value.chars().any(|character| character == '\0')
}

pub fn parse_control_request(input: &[u8]) -> Result<ControlRequest, SuStatus> {
    let wire =
        serde_json::from_slice::<ControlRequestWire>(input).map_err(|_| SuStatus::ParseError)?;

    match wire {
        ControlRequestWire::Authenticate {
            version,
            request_id,
            username,
        } if version == CONTROL_PROTOCOL_VERSION
            && request_id != 0
            && valid_username(username.trim()) =>
        {
            Ok(ControlRequest::Authenticate {
                request_id,
                username: username.trim().to_owned(),
            })
        }
        ControlRequestWire::Status {
            version,
            request_id,
        } if version == CONTROL_PROTOCOL_VERSION && request_id != 0 => {
            Ok(ControlRequest::Status { request_id })
        }
        ControlRequestWire::Cancel {
            version,
            request_id,
            target_request_id,
        } if version == CONTROL_PROTOCOL_VERSION && request_id != 0 && target_request_id != 0 => {
            Ok(ControlRequest::Cancel {
                request_id,
                target_request_id,
            })
        }
        ControlRequestWire::StorageStatus {
            version,
            request_id,
        } if version == CONTROL_PROTOCOL_VERSION && request_id != 0 => {
            Ok(ControlRequest::StorageStatus { request_id })
        }
        ControlRequestWire::ListProfiles {
            version,
            request_id,
            username,
        } if version == CONTROL_PROTOCOL_VERSION
            && request_id != 0
            && valid_username(username.trim()) =>
        {
            Ok(ControlRequest::ListProfiles {
                request_id,
                username: username.trim().to_owned(),
            })
        }
        ControlRequestWire::EnrollProfile {
            version,
            request_id,
            username,
            label,
            face_sample_source,
        } if version == CONTROL_PROTOCOL_VERSION
            && request_id != 0
            && valid_username(username.trim())
            && valid_text(label.trim(), CONTROL_PROFILE_LABEL_CAP)
            && valid_sample_source(&face_sample_source) =>
        {
            Ok(ControlRequest::EnrollProfile {
                request_id,
                username: username.trim().to_owned(),
                label: label.trim().to_owned(),
                face_sample_source,
            })
        }
        ControlRequestWire::DeleteProfile {
            version,
            request_id,
            username,
            profile_id,
        } if version == CONTROL_PROTOCOL_VERSION
            && request_id != 0
            && valid_username(username.trim())
            && valid_text(profile_id.trim(), CONTROL_PROFILE_ID_CAP) =>
        {
            Ok(ControlRequest::DeleteProfile {
                request_id,
                username: username.trim().to_owned(),
                profile_id: profile_id.trim().to_owned(),
            })
        }
        ControlRequestWire::MigrateProfiles {
            version,
            request_id,
            username,
        } if version == CONTROL_PROTOCOL_VERSION
            && request_id != 0
            && valid_username(username.trim()) =>
        {
            Ok(ControlRequest::MigrateProfiles {
                request_id,
                username: username.trim().to_owned(),
            })
        }
        ControlRequestWire::VerifyProfile {
            version,
            request_id,
            username,
            face_sample_source,
            liveness_ok,
        } if version == CONTROL_PROTOCOL_VERSION
            && request_id != 0
            && valid_username(username.trim())
            && valid_sample_source(&face_sample_source) =>
        {
            Ok(ControlRequest::VerifyProfile {
                request_id,
                username: username.trim().to_owned(),
                face_sample_source,
                liveness_ok,
            })
        }
        _ => Err(SuStatus::InvalidArgument),
    }
}

impl ControlRequest {
    fn to_ffi(&self) -> SuControlRequest {
        let mut username = [0; CONTROL_USERNAME_CAP];
        let mut profile_id = [0; CONTROL_PROFILE_ID_CAP];
        let mut label = [0; CONTROL_PROFILE_LABEL_CAP];
        let mut face_sample_source = [0; CONTROL_SAMPLE_SOURCE_CAP];
        let build = |msg_type,
                     request_id,
                     target_request_id,
                     username,
                     profile_id,
                     label,
                     face_sample_source| {
            SuControlRequest {
                msg_type,
                request_id,
                target_request_id,
                username,
                profile_id,
                label,
                face_sample_source,
                liveness_ok: false,
            }
        };
        match self {
            Self::Authenticate {
                request_id,
                username: value,
            } => {
                copy_str_to_fixed(value, &mut username);
                build(
                    SuControlMessageType::Authenticate,
                    *request_id,
                    0,
                    username,
                    profile_id,
                    label,
                    face_sample_source,
                )
            }
            Self::Status { request_id } => build(
                SuControlMessageType::Status,
                *request_id,
                0,
                username,
                profile_id,
                label,
                face_sample_source,
            ),
            Self::Cancel {
                request_id,
                target_request_id,
            } => build(
                SuControlMessageType::Cancel,
                *request_id,
                *target_request_id,
                username,
                profile_id,
                label,
                face_sample_source,
            ),
            Self::StorageStatus { request_id } => build(
                SuControlMessageType::StorageStatus,
                *request_id,
                0,
                username,
                profile_id,
                label,
                face_sample_source,
            ),
            Self::ListProfiles {
                request_id,
                username: value,
            } => {
                copy_str_to_fixed(value, &mut username);
                build(
                    SuControlMessageType::ListProfiles,
                    *request_id,
                    0,
                    username,
                    profile_id,
                    label,
                    face_sample_source,
                )
            }
            Self::EnrollProfile {
                request_id,
                username: user,
                label: profile_label,
                face_sample_source: source,
            } => {
                copy_str_to_fixed(user, &mut username);
                copy_str_to_fixed(profile_label, &mut label);
                copy_str_to_fixed(source, &mut face_sample_source);
                build(
                    SuControlMessageType::EnrollProfile,
                    *request_id,
                    0,
                    username,
                    profile_id,
                    label,
                    face_sample_source,
                )
            }
            Self::DeleteProfile {
                request_id,
                username: user,
                profile_id: id,
            } => {
                copy_str_to_fixed(user, &mut username);
                copy_str_to_fixed(id, &mut profile_id);
                build(
                    SuControlMessageType::DeleteProfile,
                    *request_id,
                    0,
                    username,
                    profile_id,
                    label,
                    face_sample_source,
                )
            }
            Self::MigrateProfiles {
                request_id,
                username: value,
            } => {
                copy_str_to_fixed(value, &mut username);
                build(
                    SuControlMessageType::MigrateProfiles,
                    *request_id,
                    0,
                    username,
                    profile_id,
                    label,
                    face_sample_source,
                )
            }
            Self::VerifyProfile {
                request_id,
                username: user,
                face_sample_source: source,
                liveness_ok,
            } => {
                copy_str_to_fixed(user, &mut username);
                copy_str_to_fixed(source, &mut face_sample_source);
                let mut request = build(
                    SuControlMessageType::VerifyProfile,
                    *request_id,
                    0,
                    username,
                    profile_id,
                    label,
                    face_sample_source,
                );
                request.liveness_ok = *liveness_ok;
                request
            }
        }
    }
}

pub fn parse_control_request_ffi(
    input: *const u8,
    input_len: usize,
    out_request: *mut SuControlRequest,
) -> SuStatus {
    if input.is_null() || out_request.is_null() {
        return SuStatus::NullArgument;
    }
    if input_len == 0 {
        return SuStatus::InvalidArgument;
    }

    let input = unsafe { slice::from_raw_parts(input, input_len) };
    match parse_control_request(input) {
        Ok(request) => {
            unsafe { out_request.write(request.to_ffi()) };
            SuStatus::Ok
        }
        Err(status) => status,
    }
}
