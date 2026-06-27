use std::ffi::{CStr, c_char};
use std::fs;
use std::hash::{Hash, Hasher};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

use crate::SuStatus;
use crate::embedding::{FaceEmbedding, mock_embedding_from_sample};

pub const PROFILE_ID_CAP: usize = 64;
pub const PROFILE_LABEL_CAP: usize = 128;

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
    pub profiles: Vec<FaceProfile>,
}

impl Default for ProfileStore {
    fn default() -> Self {
        Self {
            version: 1,
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
    serde_json::from_str::<ProfileStore>(&text).map_err(|_| SuStatus::ParseError)
}

pub fn save_store(path: &Path, store: &ProfileStore) -> Result<(), SuStatus> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|_| SuStatus::IoError)?;
    }

    let text = serde_json::to_string_pretty(store).map_err(|_| SuStatus::WriteError)?;
    let tmp_path = path.with_extension("tmp");
    fs::write(&tmp_path, text).map_err(|_| SuStatus::WriteError)?;
    fs::rename(&tmp_path, path).map_err(|_| SuStatus::WriteError)?;
    Ok(())
}

pub fn enroll_profile(
    path: &Path,
    label: &str,
    sample_seed: &str,
) -> Result<FaceProfile, SuStatus> {
    if label.is_empty() || sample_seed.is_empty() {
        return Err(SuStatus::InvalidArgument);
    }

    let mut store = load_store(path)?;
    let profile = FaceProfile {
        id: stable_profile_id(label),
        label: label.to_owned(),
        embedding: mock_embedding_from_sample(sample_seed),
        created_at_unix: now_unix(),
    };

    store.profiles.retain(|existing| existing.id != profile.id);
    store.profiles.push(profile.clone());
    save_store(path, &store)?;
    Ok(profile)
}

pub fn delete_profile(path: &Path, profile_id: &str) -> Result<bool, SuStatus> {
    if profile_id.is_empty() {
        return Err(SuStatus::InvalidArgument);
    }

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

fn stable_profile_id(label: &str) -> String {
    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    label.trim().to_lowercase().hash(&mut hasher);
    format!("{:016x}", hasher.finish())
}

pub fn copy_str_to_fixed<const N: usize>(value: &str, out: &mut [u8; N]) {
    out.fill(0);
    if N == 0 {
        return;
    }

    let bytes = value.as_bytes();
    let len = bytes.len().min(N - 1);
    out[..len].copy_from_slice(&bytes[..len]);
}

fn now_unix() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_path(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!("su_profile_store_{name}.json"))
    }

    #[test]
    fn enroll_list_delete_profile() {
        let path = test_path("enroll_list_delete");
        let _ = fs::remove_file(&path);

        let profile = enroll_profile(&path, "Alice", "face:alice:front").unwrap();
        let store = load_store(&path).unwrap();
        assert_eq!(store.profiles.len(), 1);
        assert_eq!(store.profiles[0].id, profile.id);

        assert!(delete_profile(&path, &profile.id).unwrap());
        assert!(load_store(&path).unwrap().profiles.is_empty());
        let _ = fs::remove_file(path);
    }
}
