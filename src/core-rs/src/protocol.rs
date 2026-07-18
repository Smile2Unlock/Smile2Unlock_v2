use std::slice;

use serde::Deserialize;

use crate::SuStatus;
use crate::profile::copy_str_to_fixed;

pub const CONTROL_PROTOCOL_VERSION: u32 = 1;
pub const CONTROL_USERNAME_CAP: usize = 256;

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SuControlMessageType {
    Authenticate = 1,
    Status = 2,
    Cancel = 3,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct SuControlRequest {
    pub msg_type: SuControlMessageType,
    pub request_id: u64,
    pub target_request_id: u64,
    pub username: [u8; CONTROL_USERNAME_CAP],
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
}

fn valid_username(username: &str) -> bool {
    !username.is_empty()
        && username.len() < CONTROL_USERNAME_CAP
        && !username.chars().any(char::is_control)
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
        _ => Err(SuStatus::InvalidArgument),
    }
}

impl ControlRequest {
    fn to_ffi(&self) -> SuControlRequest {
        let mut username = [0; CONTROL_USERNAME_CAP];
        match self {
            Self::Authenticate {
                request_id,
                username: value,
            } => {
                copy_str_to_fixed(value, &mut username);
                SuControlRequest {
                    msg_type: SuControlMessageType::Authenticate,
                    request_id: *request_id,
                    target_request_id: 0,
                    username,
                }
            }
            Self::Status { request_id } => SuControlRequest {
                msg_type: SuControlMessageType::Status,
                request_id: *request_id,
                target_request_id: 0,
                username,
            },
            Self::Cancel {
                request_id,
                target_request_id,
            } => SuControlRequest {
                msg_type: SuControlMessageType::Cancel,
                request_id: *request_id,
                target_request_id: *target_request_id,
                username,
            },
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
