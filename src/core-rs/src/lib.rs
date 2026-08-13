//! Smile2Unlock Rust core.
//!
//! This crate owns the pure domain logic (auth, config, embedding, pipeline,
//! profile storage) and exposes it to the C++ host through a narrow C ABI
//! defined in [`ffi`]. Domain types live here at the crate root so that both
//! the FFI layer and the internal modules share a single source of truth.

mod auth;
mod config;
mod embedding;
mod encrypted_store;
mod ffi;
mod password;
mod pipeline;
mod profile;
mod protocol;
mod storage;

#[cfg(test)]
mod tests;

// The windows-gnu std links ntdll (NtReadFile/NtCreateFile/...) and userenv
// (GetUserProfileDirectoryW) directly. Declare them here so both the xmake
// C++ link path and the standalone cdylib link resolve these symbols.
#[cfg(target_os = "windows")]
#[link(name = "ntdll")]
unsafe extern "C" {}

#[cfg(target_os = "windows")]
#[link(name = "userenv")]
unsafe extern "C" {}

use profile::{PROFILE_ID_CAP, PROFILE_LABEL_CAP};
use std::ffi::c_char;

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
    CryptoError = 9,
    KeyUnavailable = 10,
    MigrationRequired = 11,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SuAccountKind {
    LinuxUid = 1,
    WindowsSid = 2,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SuWindowsAccountKind {
    Local = 1,
    Microsoft = 2,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct SuEncryptedStoreContext {
    pub master_key: *const u8,
    pub master_key_len: usize,
    pub key_version: u32,
    pub account_kind: u32,
    pub linux_uid: u32,
    pub windows_sid: *const c_char,
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
    pub liveness_threshold: f32,
    pub preview_fps: u32,
}

pub use pipeline::{SuFaceAuthDecision, SuFaceAuthReport};
pub use protocol::{SuControlMessageType, SuControlRequest};

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct SuFaceProfileSummary {
    pub id: [u8; PROFILE_ID_CAP],
    pub label: [u8; PROFILE_LABEL_CAP],
    pub created_at_unix: u64,
}
