use std::fs;
use std::path::Path;
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use zeroize::{Zeroize, Zeroizing};

use crate::SuStatus;
use crate::encrypted_store::{DataKind, decrypt_payload, encrypt_payload};
use crate::profile::EncryptedProfileContext;
use crate::storage::atomic_write_private;

static STORE_LOCK: Mutex<()> = Mutex::new(());
const FORMAT: u16 = 1;
const VERSION: u32 = 1;
const MAX_FILE: u64 = 8192;
const MAX_SECRET: usize = 1024;

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct AuthtokRecord {
    secret: Zeroizing<String>,
    created_at: u64,
}

impl Drop for AuthtokRecord {
    fn drop(&mut self) {
        self.secret.zeroize();
    }
}

fn now() -> Result<u64, SuStatus> {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|v| v.as_secs())
        .map_err(|_| SuStatus::IoError)
}

pub(crate) fn is_enrolled(path: &Path) -> Result<bool, SuStatus> {
    match fs::symlink_metadata(path) {
        Ok(meta) if meta.is_file() => Ok(true),
        Ok(_) => Err(SuStatus::InvalidArgument),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(_) => Err(SuStatus::IoError),
    }
}

pub(crate) fn store(
    path: &Path,
    context: &EncryptedProfileContext<'_>,
    secret: &str,
) -> Result<(), SuStatus> {
    if secret.is_empty() || secret.len() > MAX_SECRET || secret.as_bytes().contains(&0) {
        return Err(SuStatus::InvalidArgument);
    }
    let _guard = STORE_LOCK.lock().map_err(|_| SuStatus::IoError)?;
    let record = AuthtokRecord {
        secret: Zeroizing::new(secret.to_owned()),
        created_at: now()?,
    };
    // Reserve the worst-case JSON escaping up front so serialization never
    // abandons a partly secret-bearing allocation during Vec growth.
    let mut plaintext = Zeroizing::new(Vec::with_capacity(MAX_FILE as usize));
    serde_json::to_writer(&mut *plaintext, &record).map_err(|_| SuStatus::WriteError)?;
    let envelope = encrypt_payload(
        context.master_key,
        context.key_version,
        &context.account,
        DataKind::Authtok,
        FORMAT,
        VERSION,
        plaintext.as_slice(),
    )
    .map_err(|_| SuStatus::CryptoError)?;
    atomic_write_private(path, &envelope)
}

pub(crate) fn load(
    path: &Path,
    context: &EncryptedProfileContext<'_>,
) -> Result<Zeroizing<Vec<u8>>, SuStatus> {
    let _guard = STORE_LOCK.lock().map_err(|_| SuStatus::IoError)?;
    let meta = fs::symlink_metadata(path).map_err(|_| SuStatus::IoError)?;
    if !meta.is_file() || meta.len() > MAX_FILE {
        return Err(SuStatus::InvalidArgument);
    }
    let envelope = fs::read(path).map_err(|_| SuStatus::IoError)?;
    let (metadata, plaintext) = decrypt_payload(
        context.master_key,
        &context.account,
        DataKind::Authtok,
        &envelope,
    )
    .map_err(|_| SuStatus::CryptoError)?;
    if metadata.payload_format != FORMAT || metadata.payload_version != VERSION {
        return Err(SuStatus::ParseError);
    }
    let record: AuthtokRecord =
        serde_json::from_slice(plaintext.as_slice()).map_err(|_| SuStatus::ParseError)?;
    if record.secret.is_empty()
        || record.secret.len() > MAX_SECRET
        || record.secret.as_bytes().contains(&0)
    {
        return Err(SuStatus::ParseError);
    }
    Ok(Zeroizing::new(record.secret.as_bytes().to_vec()))
}

pub(crate) fn clear(path: &Path) -> Result<bool, SuStatus> {
    let _guard = STORE_LOCK.lock().map_err(|_| SuStatus::IoError)?;
    match fs::symlink_metadata(path) {
        Ok(meta) if meta.is_file() => {
            fs::remove_file(path).map_err(|_| SuStatus::IoError)?;
            #[cfg(unix)]
            if let Some(parent) = path.parent() {
                fs::File::open(parent)
                    .and_then(|directory| directory.sync_all())
                    .map_err(|_| SuStatus::IoError)?;
            }
            Ok(true)
        }
        Ok(_) => Err(SuStatus::InvalidArgument),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(_) => Err(SuStatus::IoError),
    }
}
