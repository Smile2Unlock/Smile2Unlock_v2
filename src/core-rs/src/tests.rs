use std::ffi::CString;
use std::fs;
use std::path::PathBuf;

use crate::SuStatus;
use crate::auth::evaluate_auth_ffi;
use crate::config::load_config;
use crate::embedding::{
    EMBEDDING_DIM, EmbeddingBackendConfig, EmbeddingBackendKind, FaceEmbeddingError, FaceSample,
    cosine_similarity, default_embedding_backend_config, embedding_from_face_sample, normalize,
    parse_face_sample_source, try_embedding_from_face_sample,
};
use crate::pipeline::authenticate_sample;
use crate::profile::{delete_profile, enroll_profile, load_store};

fn temp_path(name: &str, extension: &str) -> PathBuf {
    std::env::temp_dir().join(format!("su_core_{name}.{extension}"))
}

fn embedding_source(values: &[f32]) -> String {
    let values = values
        .iter()
        .map(|value| value.to_string())
        .collect::<Vec<_>>()
        .join(",");
    format!("embedding:{values}")
}

fn basis_embedding(index: usize) -> Vec<f32> {
    let mut values = vec![0.0; EMBEDDING_DIM];
    values[index] = 1.0;
    values
}

#[test]
fn accepts_valid_user_above_threshold() {
    let name = CString::new("alice").unwrap();
    let decision = evaluate_auth_ffi(name.as_ptr(), 0.72, 0.65, true);

    assert_eq!(decision.status, SuStatus::Ok);
    assert!(decision.accepted);
}

#[test]
fn rejects_without_liveness() {
    let name = CString::new("alice").unwrap();
    let decision = evaluate_auth_ffi(name.as_ptr(), 0.72, 0.65, false);

    assert_eq!(decision.status, SuStatus::Ok);
    assert!(!decision.accepted);
}

#[test]
fn missing_config_file_returns_default() {
    let path = temp_path("missing_config_for_test", "toml");
    let _ = fs::remove_file(&path);

    let config = load_config(&path).unwrap();
    assert_eq!(config.version, 1);
    assert_eq!(config.preview_fps, 15);
}

#[test]
fn mock_embeddings_are_deterministic() {
    assert_eq!(
        embedding_from_face_sample("mock:face:alice:front"),
        embedding_from_face_sample("mock:face:alice:front")
    );
}

#[test]
fn cosine_similarity_is_one_for_same_embedding() {
    let embedding = embedding_from_face_sample("mock:face:alice:front").unwrap();
    assert!((cosine_similarity(&embedding, &embedding) - 1.0).abs() < 0.0001);
}

#[test]
fn rejects_empty_face_sample_source() {
    assert!(embedding_from_face_sample(" ").is_none());
    assert_eq!(
        try_embedding_from_face_sample(" "),
        Err(FaceEmbeddingError::EmptySource)
    );
}

#[test]
fn default_embedding_backend_config_uses_mock_backend() {
    assert_eq!(
        default_embedding_backend_config(),
        EmbeddingBackendConfig {
            kind: EmbeddingBackendKind::Mock
        }
    );
}

#[test]
fn parses_explicit_sample_sources() {
    assert_eq!(
        parse_face_sample_source("mock:alice"),
        Ok(FaceSample::Mock {
            id: "alice".to_owned()
        })
    );
    assert_eq!(
        parse_face_sample_source("image:/tmp/alice.png"),
        Ok(FaceSample::ImageFile {
            path: PathBuf::from("/tmp/alice.png")
        })
    );
    assert_eq!(
        parse_face_sample_source("camera:0:frame42"),
        Ok(FaceSample::CameraFrame {
            descriptor: "0:frame42".to_owned()
        })
    );
}

#[test]
fn validates_image_file_sources() {
    let path = temp_path("face_sample_source_test", "png");
    fs::write(&path, b"placeholder").unwrap();

    let source = format!("image:{}", path.display());
    assert!(embedding_from_face_sample(&source).is_some());

    let _ = fs::remove_file(path);
}

#[test]
fn rejects_missing_image_file_sources() {
    let path = temp_path("missing_face_sample_source_test", "png");
    let source = format!("image:{}", path.display());
    assert!(embedding_from_face_sample(&source).is_none());
    assert_eq!(
        try_embedding_from_face_sample(&source),
        Err(FaceEmbeddingError::InvalidImageSource)
    );
}

#[test]
fn rejects_unsupported_image_extensions() {
    let path = temp_path("face_sample_source_test", "txt");
    fs::write(&path, b"placeholder").unwrap();

    let source = format!("image:{}", path.display());
    assert!(embedding_from_face_sample(&source).is_none());

    let _ = fs::remove_file(path);
}

#[test]
fn parses_precomputed_embedding_sources() {
    let source = embedding_source(&basis_embedding(0));
    let embedding = embedding_from_face_sample(&source).unwrap();

    assert!((embedding[0] - 1.0).abs() < 0.0001);
    assert!(embedding[1..].iter().all(|value| value.abs() < 0.0001));
}

#[test]
fn normalizes_precomputed_embedding_sources() {
    let mut values = basis_embedding(0);
    values[0] = 2.0;
    let source = embedding_source(&values);
    let embedding = embedding_from_face_sample(&source).unwrap();

    assert!((embedding[0] - 1.0).abs() < 0.0001);
}

#[test]
fn rejects_wrong_length_precomputed_embedding_sources() {
    let source = embedding_source(&[1.0, 0.0]);
    assert!(embedding_from_face_sample(&source).is_none());
    assert_eq!(
        try_embedding_from_face_sample(&source),
        Err(FaceEmbeddingError::InvalidEmbeddingLength {
            expected: EMBEDDING_DIM,
            actual: 2
        })
    );
}

#[test]
fn rejects_non_numeric_precomputed_embedding_sources() {
    assert!(embedding_from_face_sample("embedding:1.0,nope").is_none());
    assert_eq!(
        try_embedding_from_face_sample("embedding:1.0,nope"),
        Err(FaceEmbeddingError::InvalidEmbeddingValue)
    );
}

#[test]
fn enroll_list_delete_profile() {
    let path = temp_path("profile_store_enroll_list_delete", "json");
    let _ = fs::remove_file(&path);

    let profile = enroll_profile(&path, "Alice", "mock:face:alice:front").unwrap();
    let store = load_store(&path).unwrap();
    assert_eq!(store.profiles.len(), 1);
    assert_eq!(store.profiles[0].id, profile.id);

    assert!(delete_profile(&path, &profile.id).unwrap());
    assert!(load_store(&path).unwrap().profiles.is_empty());
    let _ = fs::remove_file(path);
}

#[test]
fn authenticates_best_face_match() {
    let path = temp_path("auth_pipeline_best_match", "json");
    let _ = fs::remove_file(&path);
    enroll_profile(&path, "Alice", "mock:face:alice:front").unwrap();
    enroll_profile(&path, "Bob", "mock:face:bob:front").unwrap();

    let report = authenticate_sample(&path, "mock:face:alice:front", 0.80).unwrap();
    assert!(report.accepted);
    assert_eq!(report.best_profile_label.as_deref(), Some("Alice"));
    assert!(report.score > 0.99);
    let _ = fs::remove_file(path);
}

#[test]
fn rejects_when_store_is_empty() {
    let path = temp_path("auth_pipeline_empty", "json");
    let _ = fs::remove_file(&path);

    let report = authenticate_sample(&path, "mock:face:alice:front", 0.80).unwrap();
    assert!(!report.accepted);
    assert_eq!(report.profile_count, 0);
}

#[test]
fn authenticates_precomputed_embedding_match() {
    let path = temp_path("auth_pipeline_precomputed_embedding", "json");
    let _ = fs::remove_file(&path);

    let alice = embedding_source(&basis_embedding(0));
    let bob = embedding_source(&basis_embedding(1));
    enroll_profile(&path, "Alice", &alice).unwrap();
    enroll_profile(&path, "Bob", &bob).unwrap();

    let report = authenticate_sample(&path, &alice, 0.95).unwrap();
    assert!(report.accepted);
    assert_eq!(report.best_profile_label.as_deref(), Some("Alice"));
    assert!(report.score > 0.99);
    let _ = fs::remove_file(path);
}

#[test]
fn normalize_handles_zero_embedding() {
    let embedding = normalize([0.0; EMBEDDING_DIM]);
    assert_eq!(embedding, [0.0; EMBEDDING_DIM]);
}
