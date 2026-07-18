//! Smile2Unlock Rust core.
//!
//! This crate owns the pure domain logic (auth, config, embedding, pipeline,
//! profile storage) and exposes it to the C++ host through a narrow C ABI
//! defined in [`ffi`]. Domain types live here at the crate root so that both
//! the FFI layer and the internal modules share a single source of truth.

mod auth;
mod config;
mod embedding;
mod ffi;
mod pipeline;
mod profile;
mod storage;

#[cfg(test)]
mod tests;

use profile::{PROFILE_ID_CAP, PROFILE_LABEL_CAP};

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
    pub liveness_threshold: f32,
    pub preview_fps: u32,
}

pub use pipeline::{SuFaceAuthDecision, SuFaceAuthReport};

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct SuFaceProfileSummary {
    pub id: [u8; PROFILE_ID_CAP],
    pub label: [u8; PROFILE_LABEL_CAP],
    pub created_at_unix: u64,
}
