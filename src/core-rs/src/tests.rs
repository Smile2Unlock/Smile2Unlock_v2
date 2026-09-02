use std::ffi::CString;
use std::fs;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::SuStatus;
use crate::auth::evaluate_auth_ffi;
use crate::config::load_config;
use crate::embedding::{
    EMBEDDING_DIM, FaceEmbeddingError, FaceSample, cosine_similarity, embedding_from_face_sample,
    normalize, parse_face_sample_source, try_embedding_from_face_sample,
};
use crate::encrypted_store::AccountId;
use crate::pipeline::authenticate_sample_with_liveness;
use crate::profile::{
    EncryptedProfileContext, copy_str_to_fixed, delete_profile, delete_profile_encrypted,
    enroll_profile, enroll_profile_encrypted, load_encrypted_store, load_store,
    migrate_plaintext_store,
};
use crate::protocol::{ControlRequest, parse_control_request};

static TEMP_PATH_COUNTER: AtomicU64 = AtomicU64::new(0);

#[test]
fn control_protocol_parses_versioned_requests() {
    let auth = parse_control_request(
        br#"{"version":1,"msg_type":"authenticate","request_id":42,"username":" alice "}"#,
    )
    .unwrap();
    assert_eq!(
        auth,
        ControlRequest::Authenticate {
            request_id: 42,
            username: "alice".to_owned(),
        }
    );

    let status =
        parse_control_request(br#"{"version":1,"msg_type":"status","request_id":43}"#).unwrap();
    assert_eq!(status, ControlRequest::Status { request_id: 43 });

    let cancel = parse_control_request(
        br#"{"version":1,"msg_type":"cancel","request_id":44,"target_request_id":42}"#,
    )
    .unwrap();
    assert_eq!(
        cancel,
        ControlRequest::Cancel {
            request_id: 44,
            target_request_id: 42,
        }
    );

    let enroll = parse_control_request(
        br#"{"version":1,"msg_type":"enroll_profile","request_id":45,"username":"alice","label":"Front","face_sample_source":"embedding:1,0"}"#,
    )
    .unwrap();
    assert_eq!(
        enroll,
        ControlRequest::EnrollProfile {
            request_id: 45,
            username: "alice".to_owned(),
            label: "Front".to_owned(),
            face_sample_source: "embedding:1,0".to_owned(),
        }
    );

    let verify = parse_control_request(
        br#"{"version":1,"msg_type":"verify_profile","request_id":46,"username":"alice","face_sample_source":"embedding:1,0","liveness_ok":false}"#,
    )
    .unwrap();
    assert_eq!(
        verify,
        ControlRequest::VerifyProfile {
            request_id: 46,
            username: "alice".to_owned(),
            face_sample_source: "embedding:1,0".to_owned(),
            liveness_ok: false,
        }
    );
}

#[test]
fn control_protocol_rejects_untrusted_input() {
    for input in [
        br#"{"version":2,"msg_type":"status","request_id":1}"#.as_slice(),
        br#"{"version":1,"msg_type":"authenticate","request_id":0,"username":"alice"}"#,
        br#"{"version":1,"msg_type":"authenticate","request_id":1,"username":""}"#,
        br#"{"version":1,"msg_type":"authenticate","request_id":1,"username":"a\nb"}"#,
        br#"{"version":1,"msg_type":"unknown","request_id":1}"#,
    ] {
        assert!(parse_control_request(input).is_err());
    }
}

fn temp_path(name: &str, extension: &str) -> PathBuf {
    let sequence = TEMP_PATH_COUNTER.fetch_add(1, Ordering::Relaxed);
    std::env::temp_dir().join(format!(
        "su_core_{name}-{}-{sequence}.{extension}",
        std::process::id()
    ))
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
fn unsafe_config_values_fall_back_to_safe_defaults() {
    let path = temp_path("unsafe_config", "toml");
    fs::write(
        &path,
        "version = 0\nselected_camera = -1\nrecognition_threshold = -1.0\nliveness_detection = true\nliveness_threshold = 2.0\npreview_fps = 5000\n",
    )
    .unwrap();

    let config = load_config(&path).unwrap();
    assert_eq!(config.version, 1);
    assert_eq!(config.selected_camera, 0);
    assert_eq!(config.recognition_threshold, 0.65);
    assert_eq!(config.liveness_threshold, 0.50);
    assert_eq!(config.preview_fps, 15);
    let _ = fs::remove_file(path);
}

#[test]
fn invalid_auth_thresholds_are_rejected() {
    let name = CString::new("alice").unwrap();
    for threshold in [-1.0, 0.0, 1.1, f32::NAN] {
        let decision = evaluate_auth_ffi(name.as_ptr(), 1.0, threshold, true);
        assert!(!decision.accepted);
    }
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
fn accepts_arbitrary_length_precomputed_embedding_sources() {
    // Precomputed embeddings keep whatever length the caller supplies; the
    // store-level dimension lock (not the parser) is what rejects mismatched
    // dimensions. A 2-value embedding parses and normalizes like any other.
    let source = embedding_source(&[1.0, 0.0]);
    let embedding = embedding_from_face_sample(&source).unwrap();
    assert_eq!(embedding.len(), 2);
    assert!((embedding[0] - 1.0).abs() < 0.0001);
    assert!(embedding[1].abs() < 0.0001);
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

fn encrypted_context(uid: u32) -> EncryptedProfileContext<'static> {
    static MASTER_KEY: [u8; 32] = [0x5a; 32];
    EncryptedProfileContext {
        master_key: &MASTER_KEY,
        key_version: 1,
        account: AccountId::LinuxUid(uid),
    }
}

#[test]
fn encrypted_profile_store_round_trip_hides_plaintext() {
    let path = temp_path("encrypted_profile_store", "s2u");
    let _ = fs::remove_file(&path);
    let context = encrypted_context(1000);

    let profile =
        enroll_profile_encrypted(&path, &context, "Private label", "mock:face:private").unwrap();
    let bytes = fs::read(&path).unwrap();
    assert_eq!(&bytes[..4], b"S2UE");
    assert!(
        !bytes
            .windows(b"Private label".len())
            .any(|window| window == b"Private label")
    );
    assert!(serde_json::from_slice::<serde_json::Value>(&bytes).is_err());

    let store = load_encrypted_store(&path, &context).unwrap();
    assert_eq!(store.profiles.len(), 1);
    assert_eq!(store.profiles[0].id, profile.id);
    assert!(delete_profile_encrypted(&path, &context, &profile.id).unwrap());
    assert!(
        load_encrypted_store(&path, &context)
            .unwrap()
            .profiles
            .is_empty()
    );
    let _ = fs::remove_file(path);
}

#[test]
fn encrypted_profile_store_rejects_wrong_account_and_key() {
    let path = temp_path("encrypted_profile_wrong_identity", "s2u");
    let _ = fs::remove_file(&path);
    let context = encrypted_context(1000);
    enroll_profile_encrypted(&path, &context, "Alice", "mock:face:alice").unwrap();

    assert_eq!(
        load_encrypted_store(&path, &encrypted_context(1001)).unwrap_err(),
        SuStatus::CryptoError
    );
    let different_key = [0x99; 32];
    let wrong_key_context = EncryptedProfileContext {
        master_key: &different_key,
        key_version: 1,
        account: AccountId::LinuxUid(1000),
    };
    assert_eq!(
        load_encrypted_store(&path, &wrong_key_context).unwrap_err(),
        SuStatus::CryptoError
    );
    let _ = fs::remove_file(path);
}

#[test]
fn plaintext_profile_migration_verifies_and_removes_source() {
    let legacy_path = temp_path("legacy_profile_migration", "json");
    let encrypted_path = temp_path("encrypted_profile_migration", "s2u");
    let _ = fs::remove_file(&legacy_path);
    let _ = fs::remove_file(&encrypted_path);
    enroll_profile(&legacy_path, "Legacy Alice", "mock:face:alice").unwrap();

    let context = encrypted_context(1000);
    assert!(migrate_plaintext_store(&legacy_path, &encrypted_path, &context, true).unwrap());
    assert!(!legacy_path.exists());
    assert_eq!(
        load_encrypted_store(&encrypted_path, &context)
            .unwrap()
            .profiles[0]
            .label,
        "Legacy Alice"
    );
    assert!(!migrate_plaintext_store(&legacy_path, &encrypted_path, &context, true).unwrap());
    let _ = fs::remove_file(encrypted_path);
}

#[test]
fn concurrent_enrollment_preserves_all_profiles() {
    let path = Arc::new(temp_path("profile_store_concurrent", "json"));
    let _ = fs::remove_file(path.as_ref());
    let workers = (0..8)
        .map(|index| {
            let path = Arc::clone(&path);
            std::thread::spawn(move || {
                enroll_profile(
                    path.as_ref(),
                    &format!("User {index}"),
                    &format!("mock:face:user:{index}"),
                )
                .unwrap()
            })
        })
        .collect::<Vec<_>>();

    let ids = workers
        .into_iter()
        .map(|worker| worker.join().unwrap().id)
        .collect::<std::collections::HashSet<_>>();
    assert_eq!(ids.len(), 8);
    assert_eq!(load_store(path.as_ref()).unwrap().profiles.len(), 8);
    let _ = fs::remove_file(path.as_ref());
}

#[test]
fn fixed_strings_preserve_utf8_boundaries() {
    let mut output = [0_u8; 6];
    copy_str_to_fixed("你好", &mut output);
    let end = output.iter().position(|byte| *byte == 0).unwrap();
    assert_eq!(std::str::from_utf8(&output[..end]).unwrap(), "你");
}

#[cfg(unix)]
#[test]
fn profile_store_is_private_on_disk() {
    use std::os::unix::fs::PermissionsExt;

    let path = temp_path("profile_store_permissions", "json");
    enroll_profile(&path, "Alice", "mock:face:alice").unwrap();
    let mode = fs::metadata(&path).unwrap().permissions().mode() & 0o777;
    assert_eq!(mode, 0o600);
    let _ = fs::remove_file(path);
}

#[test]
fn corrupt_store_with_mixed_dimensions_is_rejected() {
    let path = temp_path("profile_store_corrupt_dimensions", "json");
    fs::write(
        &path,
        r#"{
            "version": 1,
            "embedding_dim": 2,
            "profiles": [
                {"id":"a","label":"A","embedding":[1.0,0.0],"created_at_unix":0},
                {"id":"b","label":"B","embedding":[1.0,0.0,0.0],"created_at_unix":0}
            ]
        }"#,
    )
    .unwrap();
    assert_eq!(load_store(&path).unwrap_err(), SuStatus::ParseError);
    let _ = fs::remove_file(path);
}

#[test]
fn authenticates_best_face_match() {
    let path = temp_path("auth_pipeline_best_match", "json");
    let _ = fs::remove_file(&path);
    enroll_profile(&path, "Alice", "mock:face:alice:front").unwrap();
    enroll_profile(&path, "Bob", "mock:face:bob:front").unwrap();

    let report =
        authenticate_sample_with_liveness(&path, "mock:face:alice:front", 0.80, true).unwrap();
    assert!(report.accepted);
    assert_eq!(report.best_profile_label.as_deref(), Some("Alice"));
    assert!(report.score > 0.99);
    let _ = fs::remove_file(path);
}

#[test]
fn rejects_when_store_is_empty() {
    let path = temp_path("auth_pipeline_empty", "json");
    let _ = fs::remove_file(&path);

    let report =
        authenticate_sample_with_liveness(&path, "mock:face:alice:front", 0.80, true).unwrap();
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

    let report = authenticate_sample_with_liveness(&path, &alice, 0.95, true).unwrap();
    assert!(report.accepted);
    assert_eq!(report.best_profile_label.as_deref(), Some("Alice"));
    assert!(report.score > 0.99);
    let _ = fs::remove_file(path);
}

#[test]
fn rejects_precomputed_embedding_match_without_liveness() {
    let path = temp_path("auth_pipeline_precomputed_embedding_no_liveness", "json");
    let _ = fs::remove_file(&path);

    let alice = embedding_source(&basis_embedding(0));
    enroll_profile(&path, "Alice", &alice).unwrap();

    let report = authenticate_sample_with_liveness(&path, &alice, 0.95, false).unwrap();
    assert!(!report.accepted);
    assert!(report.score > 0.99);
    assert_eq!(report.best_profile_label.as_deref(), Some("Alice"));
    assert_eq!(report.reason, "liveness check failed");
    let _ = fs::remove_file(path);
}

#[test]
fn normalize_handles_zero_embedding() {
    let embedding = normalize(vec![0.0; EMBEDDING_DIM]);
    assert_eq!(embedding, vec![0.0; EMBEDDING_DIM]);
}

#[test]
fn first_enrollment_locks_embedding_dimension() {
    let path = temp_path("profile_store_dim_lock_first", "json");
    let _ = fs::remove_file(&path);

    let three = embedding_source(&[1.0, 0.0, 0.0]);
    enroll_profile(&path, "Alice", &three).unwrap();
    let store = load_store(&path).unwrap();
    assert_eq!(store.embedding_dim, Some(3));

    let _ = fs::remove_file(path);
}

#[test]
fn rejects_enrollment_with_mismatched_dimension() {
    let path = temp_path("profile_store_dim_lock_mismatch", "json");
    let _ = fs::remove_file(&path);

    enroll_profile(&path, "Alice", &embedding_source(&[1.0, 0.0, 0.0])).unwrap();
    // A 4-dim embedding must be rejected once the store is locked to 3.
    let result = enroll_profile(&path, "Bob", &embedding_source(&[1.0, 0.0, 0.0, 0.0]));
    assert!(result.is_err());
    assert_eq!(result.unwrap_err(), SuStatus::InvalidArgument);

    let store = load_store(&path).unwrap();
    assert_eq!(store.profiles.len(), 1);
    assert_eq!(store.embedding_dim, Some(3));
    let _ = fs::remove_file(path);
}

#[test]
fn rejects_authentication_with_mismatched_probe_dimension() {
    let path = temp_path("profile_store_dim_lock_auth_mismatch", "json");
    let _ = fs::remove_file(&path);
    enroll_profile(&path, "Alice", &embedding_source(&[1.0, 0.0, 0.0])).unwrap();

    let report = authenticate_sample_with_liveness(
        &path,
        &embedding_source(&[1.0, 0.0, 0.0, 0.0]),
        0.80,
        true,
    )
    .unwrap();
    assert!(!report.accepted);
    assert_eq!(report.profile_count, 1);
    assert!(
        report.reason.contains("embedding dimension mismatch"),
        "reason was: {reason}",
        reason = report.reason
    );
    assert!(report.reason.contains("probe=4"));
    assert!(report.reason.contains("store=3"));

    let _ = fs::remove_file(path);
}

#[test]
fn authenticates_when_probe_dimension_matches_locked_store() {
    let path = temp_path("profile_store_dim_lock_auth_match", "json");
    let _ = fs::remove_file(&path);
    enroll_profile(&path, "Alice", &embedding_source(&[1.0, 0.0, 0.0])).unwrap();

    let report =
        authenticate_sample_with_liveness(&path, &embedding_source(&[1.0, 0.0, 0.0]), 0.95, true)
            .unwrap();
    assert!(report.accepted);
    assert!(report.score > 0.99);

    let _ = fs::remove_file(path);
}

#[test]
fn legacy_store_without_dim_field_backfills_on_load() {
    // A store written before embedding_dim existed: no field, but profiles
    // present. load_store must backfill the dimension from the first profile.
    let path = temp_path("profile_store_dim_legacy", "json");
    let _ = fs::remove_file(&path);

    let legacy = r#"{
        "version": 1,
        "profiles": [
            {
                "id": "legacy-alice",
                "label": "Alice",
                "embedding": [1.0, 0.0, 0.0, 0.0, 0.0],
                "created_at_unix": 0
            }
        ]
    }"#;
    fs::write(&path, legacy).unwrap();

    let store = load_store(&path).unwrap();
    assert_eq!(store.embedding_dim, Some(5));

    // Subsequent enrollment must honor the backfilled dimension.
    let same_dim = enroll_profile(&path, "Bob", &embedding_source(&[0.0, 1.0, 0.0, 0.0, 0.0]));
    assert!(same_dim.is_ok());
    // Mismatched dimension still rejected.
    let wrong_dim = enroll_profile(&path, "Carol", &embedding_source(&[0.0, 1.0, 0.0]));
    assert!(wrong_dim.is_err());
    assert_eq!(wrong_dim.unwrap_err(), SuStatus::InvalidArgument);

    let _ = fs::remove_file(path);
}
