use std::collections::{HashSet, VecDeque};
use std::fs;
use std::path::Path;
use std::sync::{LazyLock, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use zeroize::{Zeroize, Zeroizing};

use crate::encrypted_store::{
    AccountId, DataKind, EnvelopeError, decrypt_payload, encrypt_payload,
};
use crate::profile::EncryptedProfileContext;
use crate::storage::atomic_write_private;
use crate::{SuStatus, SuWindowsAccountKind};

const PASSWORD_PAYLOAD_FORMAT_JSON: u16 = 2;
const PASSWORD_PAYLOAD_VERSION: u32 = 1;
const MAX_USERNAME_BYTES: usize = 512;
pub(crate) const MAX_PASSWORD_UNITS: usize = 512;
const REPLAY_CACHE_CAPACITY: usize = 1024;

static USED_REQUESTS: LazyLock<Mutex<RequestReplayCache>> =
    LazyLock::new(|| Mutex::new(RequestReplayCache::new()));
static PASSWORD_STORE_LOCK: Mutex<()> = Mutex::new(());

#[derive(Debug, Serialize, Deserialize)]
struct LogonSecret {
    version: u32,
    sid: String,
    account_kind: u8,
    canonical_username: String,
    password_utf16le: Vec<u8>,
    created_at_unix: u64,
    credential_generation: u64,
    stale: bool,
}

struct RequestReplayCache {
    order: VecDeque<(String, u64, u32)>,
    used: HashSet<(String, u64, u32)>,
}

impl RequestReplayCache {
    fn new() -> Self {
        Self {
            order: VecDeque::new(),
            used: HashSet::new(),
        }
    }

    fn insert(&mut self, request: (String, u64, u32)) {
        if !self.used.insert(request.clone()) {
            return;
        }
        self.order.push_back(request);
        while self.order.len() > REPLAY_CACHE_CAPACITY {
            if let Some(expired) = self.order.pop_front() {
                self.used.remove(&expired);
            }
        }
    }

    fn reserve(&mut self, request: (String, u64, u32)) -> bool {
        if self.used.contains(&request) {
            return false;
        }
        self.insert(request);
        true
    }
}

fn map_envelope_error(error: EnvelopeError) -> SuStatus {
    match error {
        EnvelopeError::AuthenticationFailed | EnvelopeError::KeyDerivationFailed => {
            SuStatus::CryptoError
        }
        EnvelopeError::RandomUnavailable => SuStatus::KeyUnavailable,
        EnvelopeError::InvalidAccount
        | EnvelopeError::InvalidHeader
        | EnvelopeError::UnsupportedVersion
        | EnvelopeError::UnsupportedDataKind
        | EnvelopeError::PayloadTooLarge => SuStatus::ParseError,
    }
}

fn windows_sid<'a>(context: &'a EncryptedProfileContext<'_>) -> Result<&'a str, SuStatus> {
    match &context.account {
        AccountId::WindowsSid(sid) => Ok(sid),
        AccountId::LinuxUid(_) => Err(SuStatus::InvalidArgument),
    }
}

fn password_to_bytes(password: &[u16]) -> Result<Zeroizing<Vec<u8>>, SuStatus> {
    if password.is_empty()
        || password.len() > MAX_PASSWORD_UNITS
        || password.contains(&0)
        || String::from_utf16(password).is_err()
    {
        return Err(SuStatus::InvalidArgument);
    }
    let mut bytes = Zeroizing::new(Vec::with_capacity(password.len() * 2));
    for unit in password {
        bytes.extend_from_slice(&unit.to_le_bytes());
    }
    Ok(bytes)
}

fn bytes_to_password(bytes: &[u8]) -> Result<Zeroizing<Vec<u16>>, SuStatus> {
    if bytes.is_empty() || !bytes.len().is_multiple_of(2) || bytes.len() / 2 > MAX_PASSWORD_UNITS {
        return Err(SuStatus::ParseError);
    }
    let password = bytes
        .chunks_exact(2)
        .map(|chunk| u16::from_le_bytes([chunk[0], chunk[1]]))
        .collect::<Vec<_>>();
    if password.contains(&0) || String::from_utf16(&password).is_err() {
        return Err(SuStatus::ParseError);
    }
    Ok(Zeroizing::new(password))
}

fn load_secret(
    path: &Path,
    context: &EncryptedProfileContext<'_>,
) -> Result<LogonSecret, SuStatus> {
    let envelope = fs::read(path).map_err(|_| SuStatus::IoError)?;
    let (metadata, payload) = decrypt_payload(
        context.master_key,
        &context.account,
        DataKind::WindowsPassword,
        &envelope,
    )
    .map_err(map_envelope_error)?;
    if metadata.payload_format != PASSWORD_PAYLOAD_FORMAT_JSON
        || metadata.payload_version != PASSWORD_PAYLOAD_VERSION
    {
        return Err(SuStatus::MigrationRequired);
    }
    let secret =
        serde_json::from_slice::<LogonSecret>(&payload).map_err(|_| SuStatus::ParseError)?;
    if secret.version != PASSWORD_PAYLOAD_VERSION
        || secret.sid != windows_sid(context)?
        || !matches!(
            secret.account_kind,
            value if value == SuWindowsAccountKind::Local as u8
                || value == SuWindowsAccountKind::Microsoft as u8
        )
        || secret.canonical_username.is_empty()
        || secret.canonical_username.len() > MAX_USERNAME_BYTES
        || secret.credential_generation == 0
        || bytes_to_password(&secret.password_utf16le).is_err()
    {
        return Err(SuStatus::ParseError);
    }
    Ok(secret)
}

fn save_secret(
    path: &Path,
    context: &EncryptedProfileContext<'_>,
    secret: &LogonSecret,
) -> Result<(), SuStatus> {
    let payload = Zeroizing::new(serde_json::to_vec(secret).map_err(|_| SuStatus::WriteError)?);
    let envelope = encrypt_payload(
        context.master_key,
        context.key_version,
        &context.account,
        DataKind::WindowsPassword,
        PASSWORD_PAYLOAD_FORMAT_JSON,
        PASSWORD_PAYLOAD_VERSION,
        &payload,
    )
    .map_err(map_envelope_error)?;
    atomic_write_private(path, &envelope)
}

pub(crate) fn store_logon_secret(
    path: &Path,
    context: &EncryptedProfileContext<'_>,
    account_kind: SuWindowsAccountKind,
    canonical_username: &str,
    password: &[u16],
) -> Result<u64, SuStatus> {
    if canonical_username.is_empty()
        || canonical_username.len() > MAX_USERNAME_BYTES
        || canonical_username.chars().any(char::is_control)
    {
        return Err(SuStatus::InvalidArgument);
    }
    let _guard = PASSWORD_STORE_LOCK.lock().map_err(|_| SuStatus::IoError)?;
    let sid = windows_sid(context)?.to_owned();
    let password_utf16le = password_to_bytes(password)?;
    let previous_generation = if path.exists() {
        load_secret(path, context)?.credential_generation
    } else {
        0
    };
    let generation = previous_generation
        .checked_add(1)
        .ok_or(SuStatus::InvalidArgument)?;
    let mut secret = LogonSecret {
        version: PASSWORD_PAYLOAD_VERSION,
        sid,
        account_kind: account_kind as u8,
        canonical_username: canonical_username.to_owned(),
        password_utf16le: password_utf16le.to_vec(),
        created_at_unix: SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|duration| duration.as_secs())
            .unwrap_or_default(),
        credential_generation: generation,
        stale: false,
    };
    let result = save_secret(path, context, &secret);
    secret.password_utf16le.zeroize();
    result.map(|_| generation)
}

pub(crate) fn prepare_logon_secret(
    path: &Path,
    context: &EncryptedProfileContext<'_>,
    request_id: u64,
    logon_session_id: u32,
) -> Result<Zeroizing<Vec<u16>>, SuStatus> {
    if request_id == 0 {
        return Err(SuStatus::InvalidArgument);
    }
    let request = (
        windows_sid(context)?.to_owned(),
        request_id,
        logon_session_id,
    );
    // Reserve the request id before touching the secret. The check and insert
    // must be one operation if the pipe server starts serving concurrently.
    {
        let mut replay_cache = USED_REQUESTS.lock().map_err(|_| SuStatus::IoError)?;
        if !replay_cache.reserve(request) {
            return Err(SuStatus::UserDenied);
        }
    }
    let mut secret = load_secret(path, context)?;
    if secret.stale {
        secret.password_utf16le.zeroize();
        return Err(SuStatus::UserDenied);
    }
    let password = bytes_to_password(&secret.password_utf16le)?;
    secret.password_utf16le.zeroize();
    Ok(password)
}

pub(crate) fn mark_logon_secret_stale(
    path: &Path,
    context: &EncryptedProfileContext<'_>,
) -> Result<(), SuStatus> {
    let _guard = PASSWORD_STORE_LOCK.lock().map_err(|_| SuStatus::IoError)?;
    let mut secret = load_secret(path, context)?;
    secret.stale = true;
    let result = save_secret(path, context, &secret);
    secret.password_utf16le.zeroize();
    result
}

pub(crate) fn clear_logon_secret(
    path: &Path,
    context: &EncryptedProfileContext<'_>,
) -> Result<bool, SuStatus> {
    let _guard = PASSWORD_STORE_LOCK.lock().map_err(|_| SuStatus::IoError)?;
    if !path.exists() {
        return Ok(false);
    }
    let mut secret = load_secret(path, context)?;
    secret.password_utf16le.zeroize();
    fs::remove_file(path).map_err(|_| SuStatus::WriteError)?;
    #[cfg(unix)]
    if let Some(parent) = path.parent() {
        fs::File::open(parent)
            .and_then(|directory| directory.sync_all())
            .map_err(|_| SuStatus::WriteError)?;
    }
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn context() -> EncryptedProfileContext<'static> {
        static KEY: [u8; 32] = [0xa5; 32];
        EncryptedProfileContext {
            master_key: &KEY,
            key_version: 1,
            account: AccountId::WindowsSid("S-1-5-21-1000".to_owned()),
        }
    }

    fn temporary_path(name: &str) -> std::path::PathBuf {
        std::env::temp_dir().join(format!(
            "smile2unlock-{name}-{}-{}.s2u",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ))
    }

    #[test]
    fn password_round_trip_is_one_time_and_not_plaintext() {
        let path = temporary_path("password");
        let password = "P@ssw0rd".encode_utf16().collect::<Vec<_>>();
        assert_eq!(
            store_logon_secret(
                &path,
                &context(),
                SuWindowsAccountKind::Microsoft,
                "MicrosoftAccount\\user@example.com",
                &password,
            )
            .unwrap(),
            1
        );
        let bytes = fs::read(&path).unwrap();
        assert_eq!(&bytes[..4], b"S2UE");
        assert!(!bytes.windows(8).any(|window| window == b"P@ssw0rd"));

        assert_eq!(
            prepare_logon_secret(&path, &context(), 100, 7)
                .unwrap()
                .as_slice(),
            password
        );
        assert_eq!(
            prepare_logon_secret(&path, &context(), 100, 7).unwrap_err(),
            SuStatus::UserDenied
        );
        let _ = fs::remove_file(path);
    }

    #[test]
    fn stale_password_cannot_be_prepared_until_replaced() {
        let path = temporary_path("stale-password");
        let password = "old password".encode_utf16().collect::<Vec<_>>();
        store_logon_secret(
            &path,
            &context(),
            SuWindowsAccountKind::Local,
            "MACHINE\\alice",
            &password,
        )
        .unwrap();
        mark_logon_secret_stale(&path, &context()).unwrap();
        assert_eq!(
            prepare_logon_secret(&path, &context(), 101, 7).unwrap_err(),
            SuStatus::UserDenied
        );

        let replacement = "new password".encode_utf16().collect::<Vec<_>>();
        assert_eq!(
            store_logon_secret(
                &path,
                &context(),
                SuWindowsAccountKind::Local,
                "MACHINE\\alice",
                &replacement,
            )
            .unwrap(),
            2
        );
        assert_eq!(
            prepare_logon_secret(&path, &context(), 102, 7)
                .unwrap()
                .as_slice(),
            replacement
        );
        assert!(clear_logon_secret(&path, &context()).unwrap());
        assert!(!path.exists());
    }
}
