use std::collections::HashSet;
use std::ffi::{CStr, c_char};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

use crate::SuStatus;
use crate::embedding::{FaceEmbedding, embedding_from_face_sample};
use crate::storage::atomic_write_private;

pub const PROFILE_ID_CAP: usize = 64;
pub const PROFILE_LABEL_CAP: usize = 128;

static PROFILE_STORE_LOCK: Mutex<()> = Mutex::new(());

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FaceProfile {
    pub id: String,
    pub label: String,
    pub embedding: FaceEmbedding,
    pub created_at_unix: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProfileStore {
    pub version: u32,
    #[serde(default)]
    pub embedding_dim: Option<u32>,
    pub profiles: Vec<FaceProfile>,
}

impl Default for ProfileStore {
    fn default() -> Self {
        Self {
            version: 1,
            embedding_dim: None,
            profiles: Vec::new(),
        }
    }
}

pub fn path_from_ptr(path: *const c_char) -> Result<PathBuf, SuStatus> {
    if path.is_null() {
        return Err(SuStatus::NullArgument);
    }

    let raw = unsafe { CStr::from_ptr(path) };
    raw.to_str()
        .map(PathBuf::from)
        .map_err(|_| SuStatus::InvalidUtf8)
}

pub fn string_from_ptr(value: *const c_char) -> Result<String, SuStatus> {
    if value.is_null() {
        return Err(SuStatus::NullArgument);
    }

    let raw = unsafe { CStr::from_ptr(value) };
    raw.to_str()
        .map(str::trim)
        .map(str::to_owned)
        .map_err(|_| SuStatus::InvalidUtf8)
}

pub fn load_store(path: &Path) -> Result<ProfileStore, SuStatus> {
    if !path.exists() {
        return Ok(ProfileStore::default());
    }

    let text = fs::read_to_string(path).map_err(|_| SuStatus::IoError)?;
    let mut store =
        serde_json::from_str::<ProfileStore>(&text).map_err(|_| SuStatus::ParseError)?;

    // Legacy stores created before the embedding_dim field carry no dimension.
    // Backfill from the first profile so the lock applies going forward; the
    // existing profiles are by construction consistent with that dimension.
    if store.embedding_dim.is_none() && !store.profiles.is_empty() {
        store.embedding_dim = Some(store.profiles[0].embedding.len() as u32);
    }
    if !valid_store(&store) {
        return Err(SuStatus::ParseError);
    }
    Ok(store)
}

fn valid_store(store: &ProfileStore) -> bool {
    let Some(expected_dim) = store.embedding_dim.map(|dim| dim as usize) else {
        return store.profiles.is_empty();
    };
    if expected_dim == 0 {
        return false;
    }
    let mut ids = HashSet::with_capacity(store.profiles.len());
    store.profiles.iter().all(|profile| {
        !profile.id.is_empty()
            && !profile.label.is_empty()
            && ids.insert(profile.id.as_str())
            && profile.embedding.len() == expected_dim
            && profile.embedding.iter().all(|value| value.is_finite())
    })
}

pub fn save_store(path: &Path, store: &ProfileStore) -> Result<(), SuStatus> {
    if !valid_store(store) {
        return Err(SuStatus::InvalidArgument);
    }
    let text = serde_json::to_string_pretty(store).map_err(|_| SuStatus::WriteError)?;
    atomic_write_private(path, text.as_bytes())
}

pub fn enroll_profile(
    path: &Path,
    label: &str,
    face_sample_source: &str,
) -> Result<FaceProfile, SuStatus> {
    let Some(embedding) = embedding_from_face_sample(face_sample_source) else {
        return Err(SuStatus::InvalidArgument);
    };
    if label.is_empty() {
        return Err(SuStatus::InvalidArgument);
    }

    let _guard = PROFILE_STORE_LOCK.lock().map_err(|_| SuStatus::IoError)?;
    let mut store = load_store(path)?;

    // Lock the embedding dimension at the first enrollment so a store never
    // mixes embeddings from incompatible backends (e.g. mock 32-dim and
    // SeetaFace ~512-dim). Subsequent enrollments must match the locked dim.
    match store.embedding_dim {
        None => store.embedding_dim = Some(embedding.len() as u32),
        Some(dim) if dim as usize != embedding.len() => {
            return Err(SuStatus::InvalidArgument);
        }
        Some(_) => {}
    }

    let profile = FaceProfile {
        id: unique_profile_id(),
        label: label.to_owned(),
        embedding,
        created_at_unix: now_unix(),
    };

    store.profiles.push(profile.clone());
    save_store(path, &store)?;
    Ok(profile)
}

pub fn delete_profile(path: &Path, profile_id: &str) -> Result<bool, SuStatus> {
    if profile_id.is_empty() {
        return Err(SuStatus::InvalidArgument);
    }

    let _guard = PROFILE_STORE_LOCK.lock().map_err(|_| SuStatus::IoError)?;
    let mut store = load_store(path)?;
    let before = store.profiles.len();
    store.profiles.retain(|profile| profile.id != profile_id);
    let deleted = store.profiles.len() != before;
    save_store(path, &store)?;
    Ok(deleted)
}

pub fn list_profiles_json(path: &Path) -> Result<String, SuStatus> {
    let store = load_store(path)?;
    serde_json::to_string_pretty(&store).map_err(|_| SuStatus::WriteError)
}

pub fn list_profiles(path: &Path) -> Result<Vec<FaceProfile>, SuStatus> {
    Ok(load_store(path)?.profiles)
}

// Generate a profile id from epoch nanoseconds, process id, and a per-process
// counter. The components remain visible instead of being compressed through a
// non-stable standard-library hasher, and fit in the 64-byte FFI field.
// It is independent of the label. Labels are display-only and must never become the
// profile identity (a re-enrollment of the same label is a new profile, not an
// overwrite of the old one).
fn unique_profile_id() -> String {
    use std::sync::atomic::{AtomicU64, Ordering};
    static COUNTER: AtomicU64 = AtomicU64::new(0);

    let seq = COUNTER.fetch_add(1, Ordering::Relaxed);
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_nanos())
        .unwrap_or_default();
    format!("{nanos:032x}-{:08x}-{seq:016x}", std::process::id())
}

pub fn copy_str_to_fixed<const N: usize>(value: &str, out: &mut [u8; N]) {
    out.fill(0);
    if N == 0 {
        return;
    }

    let bytes = value.as_bytes();
    let mut len = bytes.len().min(N - 1);
    while !value.is_char_boundary(len) {
        len -= 1;
    }
    out[..len].copy_from_slice(&bytes[..len]);
}

fn now_unix() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or_default()
}
