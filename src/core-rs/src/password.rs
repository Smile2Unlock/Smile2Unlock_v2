use std::collections::{HashSet, VecDeque};
use std::fs;
use std::path::Path;
use std::sync::{LazyLock, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use zeroize::Zeroizing;

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

#[derive(Serialize, Deserialize)]
struct LogonSecret {
    version: u32,
    sid: String,
    account_kind: u8,
    canonical_username: String,
    #[serde(
        serialize_with = "serialize_password",
        deserialize_with = "deserialize_password"
    )]
    password_utf16le: Zeroizing<Vec<u8>>,
    created_at_unix: u64,
    credential_generation: u64,
    stale: bool,
}

// Keep the serialized byte-array format unchanged, but own the plaintext
// through a zeroizing guard even if a later JSON field fails to deserialize.
fn serialize_password<S: serde::Serializer>(
    password: &Zeroizing<Vec<u8>>,
    serializer: S,
) -> Result<S::Ok, S::Error> {
    password.as_slice().serialize(serializer)
}

fn deserialize_password<'de, D: serde::Deserializer<'de>>(
    deserializer: D,
) -> Result<Zeroizing<Vec<u8>>, D::Error> {
    struct PasswordVisitor;
    impl<'de> serde::de::Visitor<'de> for PasswordVisitor {
        type Value = Zeroizing<Vec<u8>>;

        fn expecting(&self, formatter: &mut std::fmt::Formatter) -> std::fmt::Result {
            formatter.write_str("a bounded UTF-16LE password byte array")
        }

        fn visit_seq<A: serde::de::SeqAccess<'de>>(
            self,
            mut seq: A,
        ) -> Result<Self::Value, A::Error> {
            // Fixed capacity prevents reallocation from leaving old plaintext
            // allocations behind. The guard also covers partial JSON errors.
            let mut bytes = Zeroizing::new(Vec::with_capacity(MAX_PASSWORD_UNITS * 2));
            while let Some(byte) = seq.next_element::<u8>()? {
                if bytes.len() == MAX_PASSWORD_UNITS * 2 {
                    return Err(serde::de::Error::custom("password byte array is too long"));
                }
                bytes.push(byte);
            }
            Ok(bytes)
        }
    }
    deserializer.deserialize_seq(PasswordVisitor)
}

fn valid_password(password: &[u16]) -> bool {
    !password.is_empty()
        && password.len() <= MAX_PASSWORD_UNITS
        && !password.contains(&0)
        && std::char::decode_utf16(password.iter().copied()).all(|unit| unit.is_ok())
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
    if !valid_password(password) {
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
    let password = Zeroizing::new(
        bytes
            .chunks_exact(2)
            .map(|chunk| u16::from_le_bytes([chunk[0], chunk[1]]))
            .collect::<Vec<_>>(),
    );
    if !valid_password(&password) {
        return Err(SuStatus::ParseError);
    }
    Ok(password)
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
    // Each JSON byte value uses at most three digits plus a comma; strings
    // use at most six output bytes per input byte. Reserve the full bound so
    // encoding never reallocates a buffer that already contains plaintext.
    let capacity = secret.password_utf16le.len() * 4
        + (secret.sid.len() + secret.canonical_username.len()) * 6
        + 512;
    let mut payload = Zeroizing::new(Vec::with_capacity(capacity));
    serde_json::to_writer(&mut *payload, secret).map_err(|_| SuStatus::WriteError)?;
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
    let secret = LogonSecret {
        version: PASSWORD_PAYLOAD_VERSION,
        sid,
        account_kind: account_kind as u8,
        canonical_username: canonical_username.to_owned(),
        password_utf16le,
        created_at_unix: SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|duration| duration.as_secs())
            .unwrap_or_default(),
        credential_generation: generation,
        stale: false,
    };
    let result = save_secret(path, context, &secret);
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
    let secret = load_secret(path, context)?;
    if secret.stale {
        return Err(SuStatus::UserDenied);
    }
    let password = bytes_to_password(&secret.password_utf16le)?;
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
    let _secret = load_secret(path, context)?;
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
    fn utf16_password_validation_preserves_unicode_and_rejects_invalid_units() {
        let unicode = "密碼😀".encode_utf16().collect::<Vec<_>>();
        let bytes = password_to_bytes(&unicode).unwrap();
        assert_eq!(bytes_to_password(&bytes).unwrap().as_slice(), unicode);
        for invalid in [vec![0xd800], vec![0xdc00], vec![0], vec![0xd800, 65]] {
            assert!(matches!(
                password_to_bytes(&invalid),
                Err(SuStatus::InvalidArgument)
            ));
            let bytes = invalid
                .iter()
                .flat_map(|unit| unit.to_le_bytes())
                .collect::<Vec<_>>();
            assert!(matches!(
                bytes_to_password(&bytes),
                Err(SuStatus::ParseError)
            ));
        }
        assert!(bytes_to_password(&[1]).is_err());
        assert!(password_to_bytes(&vec![65; MAX_PASSWORD_UNITS]).is_ok());
        assert!(password_to_bytes(&vec![65; MAX_PASSWORD_UNITS + 1]).is_err());
    }

    #[test]
    fn password_json_keeps_legacy_format_and_bounds_partial_deserialization() {
        let password =
            deserialize_password(&mut serde_json::Deserializer::from_str("[65,0,66,0]")).unwrap();
        assert_eq!(password.as_slice(), [65, 0, 66, 0]);
        let mut encoded = Zeroizing::new(Vec::with_capacity(32));
        serialize_password(&password, &mut serde_json::Serializer::new(&mut *encoded)).unwrap();
        assert_eq!(encoded.as_slice(), b"[65,0,66,0]");
        // The guard remains active while other fields are decoded and on errors.
        fn assert_zeroizes_on_drop<T: zeroize::ZeroizeOnDrop>(_: &T) {}
        assert_zeroizes_on_drop(&password);
        for invalid in ["[65,0,999]", "[65,0,\"bad\"]", "[65,0,", "null"] {
            assert!(
                deserialize_password(&mut serde_json::Deserializer::from_str(invalid)).is_err()
            );
        }
        let oversized = serde_json::to_string(&vec![65u8; MAX_PASSWORD_UNITS * 2 + 1]).unwrap();
        assert!(deserialize_password(&mut serde_json::Deserializer::from_str(&oversized)).is_err());
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
