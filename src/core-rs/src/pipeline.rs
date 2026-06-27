use std::cmp::Ordering;
use std::path::Path;

use serde::Serialize;

use crate::SuStatus;
use crate::embedding::{cosine_similarity, embedding_from_face_sample};
use crate::profile::{
    FaceProfile, PROFILE_ID_CAP, PROFILE_LABEL_CAP, copy_str_to_fixed, load_store,
};

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct SuFaceAuthDecision {
    pub status: SuStatus,
    pub accepted: bool,
    pub score: f32,
    pub profile_count: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct SuFaceAuthReport {
    pub status: SuStatus,
    pub accepted: bool,
    pub score: f32,
    pub threshold: f32,
    pub profile_count: u32,
    pub best_profile_id: [u8; PROFILE_ID_CAP],
    pub best_profile_label: [u8; PROFILE_LABEL_CAP],
    pub reason: [u8; 128],
}

#[derive(Debug, Clone, Serialize)]
pub struct FaceAuthReport {
    pub accepted: bool,
    pub score: f32,
    pub threshold: f32,
    pub profile_count: usize,
    pub best_profile_id: Option<String>,
    pub best_profile_label: Option<String>,
    pub reason: String,
}

impl FaceAuthReport {
    pub(crate) fn to_ffi(&self, status: SuStatus) -> SuFaceAuthReport {
        let mut best_profile_id = [0; PROFILE_ID_CAP];
        let mut best_profile_label = [0; PROFILE_LABEL_CAP];
        let mut reason = [0; 128];

        if let Some(id) = &self.best_profile_id {
            copy_str_to_fixed(id, &mut best_profile_id);
        }
        if let Some(label) = &self.best_profile_label {
            copy_str_to_fixed(label, &mut best_profile_label);
        }
        copy_str_to_fixed(&self.reason, &mut reason);

        SuFaceAuthReport {
            status,
            accepted: self.accepted,
            score: self.score,
            threshold: self.threshold,
            profile_count: self.profile_count as u32,
            best_profile_id,
            best_profile_label,
            reason,
        }
    }
}

pub fn authenticate_sample(
    store_path: &Path,
    face_sample_source: &str,
    threshold: f32,
) -> Result<FaceAuthReport, SuStatus> {
    let Some(probe) = embedding_from_face_sample(face_sample_source) else {
        return Err(SuStatus::InvalidArgument);
    };

    let store = load_store(store_path)?;
    if store.profiles.is_empty() {
        return Ok(FaceAuthReport {
            accepted: false,
            score: 0.0,
            threshold,
            profile_count: 0,
            best_profile_id: None,
            best_profile_label: None,
            reason: "no enrolled profiles".to_owned(),
        });
    }

    let best = store
        .profiles
        .iter()
        .map(|profile| (profile, cosine_similarity(&probe, &profile.embedding)))
        .max_by(|left, right| left.1.partial_cmp(&right.1).unwrap_or(Ordering::Equal));

    Ok(match best {
        Some((profile, score)) => report_for_match(profile, score, threshold, store.profiles.len()),
        None => FaceAuthReport {
            accepted: false,
            score: 0.0,
            threshold,
            profile_count: store.profiles.len(),
            best_profile_id: None,
            best_profile_label: None,
            reason: "no comparable profiles".to_owned(),
        },
    })
}

pub fn authenticate_sample_ffi(
    store_path: &Path,
    face_sample_source: &str,
    threshold: f32,
) -> SuFaceAuthDecision {
    match authenticate_sample(store_path, face_sample_source, threshold) {
        Ok(report) => SuFaceAuthDecision {
            status: SuStatus::Ok,
            accepted: report.accepted,
            score: report.score,
            profile_count: report.profile_count as u32,
        },
        Err(status) => SuFaceAuthDecision {
            status,
            accepted: false,
            score: 0.0,
            profile_count: 0,
        },
    }
}

pub fn authenticate_sample_report_json(
    store_path: &Path,
    face_sample_source: &str,
    threshold: f32,
) -> Result<String, SuStatus> {
    let report = authenticate_sample(store_path, face_sample_source, threshold)?;
    serde_json::to_string_pretty(&report).map_err(|_| SuStatus::WriteError)
}

pub fn authenticate_sample_report_ffi(
    store_path: &Path,
    face_sample_source: &str,
    threshold: f32,
) -> SuFaceAuthReport {
    match authenticate_sample(store_path, face_sample_source, threshold) {
        Ok(report) => report.to_ffi(SuStatus::Ok),
        Err(status) => FaceAuthReport {
            accepted: false,
            score: 0.0,
            threshold,
            profile_count: 0,
            best_profile_id: None,
            best_profile_label: None,
            reason: "face authentication failed".to_owned(),
        }
        .to_ffi(status),
    }
}

fn report_for_match(
    profile: &FaceProfile,
    score: f32,
    threshold: f32,
    profile_count: usize,
) -> FaceAuthReport {
    let accepted = score >= threshold;
    FaceAuthReport {
        accepted,
        score,
        threshold,
        profile_count,
        best_profile_id: Some(profile.id.clone()),
        best_profile_label: Some(profile.label.clone()),
        reason: if accepted {
            "best face match passed threshold".to_owned()
        } else {
            "best face match below threshold".to_owned()
        },
    }
}
