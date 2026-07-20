use chacha20poly1305::aead::{Aead, KeyInit, Payload};
use chacha20poly1305::{Key, XChaCha20Poly1305, XNonce};
use hkdf::Hkdf;
use sha2::{Digest, Sha256};
use zeroize::{Zeroize, Zeroizing};

const MAGIC: &[u8; 4] = b"S2UE";
const ENVELOPE_VERSION: u16 = 1;
const HEADER_LEN: usize = 100;
const SALT_LEN: usize = 16;
const NONCE_LEN: usize = 24;
const TAG_LEN: usize = 16;
const MAX_CIPHERTEXT_LEN: usize = 16 * 1024 * 1024;
const PROFILE_PAYLOAD_FORMAT_JSON: u16 = 1;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum AccountId {
    LinuxUid(u32),
    WindowsSid(String),
}

impl AccountId {
    fn kind(&self) -> u8 {
        match self {
            Self::LinuxUid(_) => 1,
            Self::WindowsSid(_) => 2,
        }
    }

    fn canonical_bytes(&self) -> Vec<u8> {
        match self {
            Self::LinuxUid(uid) => uid.to_be_bytes().to_vec(),
            Self::WindowsSid(sid) => sid.as_bytes().to_vec(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum DataKind {
    Profile = 1,
    WindowsPassword = 2,
}

impl TryFrom<u8> for DataKind {
    type Error = EnvelopeError;

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            1 => Ok(Self::Profile),
            2 => Ok(Self::WindowsPassword),
            _ => Err(EnvelopeError::UnsupportedDataKind),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum EnvelopeError {
    InvalidAccount,
    InvalidHeader,
    UnsupportedVersion,
    UnsupportedDataKind,
    PayloadTooLarge,
    RandomUnavailable,
    KeyDerivationFailed,
    AuthenticationFailed,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct EnvelopeMetadata {
    pub payload_format: u16,
    pub payload_version: u32,
    pub key_version: u32,
    pub data_kind: DataKind,
}

struct ParsedHeader {
    metadata: EnvelopeMetadata,
    account_kind: u8,
    account_hash: [u8; 32],
    salt: [u8; SALT_LEN],
    nonce: [u8; NONCE_LEN],
    ciphertext_len: usize,
}

fn account_hash(account: &AccountId) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update([account.kind()]);
    hasher.update(account.canonical_bytes());
    hasher.finalize().into()
}

fn append_length_prefixed(destination: &mut Vec<u8>, value: &[u8]) {
    destination.extend_from_slice(&(value.len() as u32).to_be_bytes());
    destination.extend_from_slice(value);
}

fn derive_key(
    master_key: &[u8; 32],
    account: &AccountId,
    data_kind: DataKind,
    key_version: u32,
    salt: &[u8; SALT_LEN],
) -> Result<Zeroizing<[u8; 32]>, EnvelopeError> {
    let hkdf = Hkdf::<Sha256>::new(Some(salt), master_key);
    let mut info = Vec::with_capacity(64);
    append_length_prefixed(&mut info, MAGIC);
    info.extend_from_slice(&ENVELOPE_VERSION.to_be_bytes());
    info.push(account.kind());
    append_length_prefixed(&mut info, &account.canonical_bytes());
    info.push(data_kind as u8);
    info.extend_from_slice(&key_version.to_be_bytes());

    let mut key = Zeroizing::new([0_u8; 32]);
    hkdf.expand(&info, key.as_mut())
        .map_err(|_| EnvelopeError::KeyDerivationFailed)?;
    info.zeroize();
    Ok(key)
}

fn build_header(
    metadata: EnvelopeMetadata,
    account: &AccountId,
    salt: &[u8; SALT_LEN],
    nonce: &[u8; NONCE_LEN],
    ciphertext_len: usize,
) -> Result<[u8; HEADER_LEN], EnvelopeError> {
    let ciphertext_len =
        u64::try_from(ciphertext_len).map_err(|_| EnvelopeError::PayloadTooLarge)?;
    let mut header = [0_u8; HEADER_LEN];
    header[0..4].copy_from_slice(MAGIC);
    header[4..6].copy_from_slice(&ENVELOPE_VERSION.to_be_bytes());
    header[6..8].copy_from_slice(&metadata.payload_format.to_be_bytes());
    header[8..12].copy_from_slice(&metadata.payload_version.to_be_bytes());
    header[12..16].copy_from_slice(&metadata.key_version.to_be_bytes());
    header[16] = account.kind();
    header[17] = metadata.data_kind as u8;
    header[20..52].copy_from_slice(&account_hash(account));
    header[52..68].copy_from_slice(salt);
    header[68..92].copy_from_slice(nonce);
    header[92..100].copy_from_slice(&ciphertext_len.to_be_bytes());
    Ok(header)
}

fn parse_header(envelope: &[u8]) -> Result<ParsedHeader, EnvelopeError> {
    if envelope.len() < HEADER_LEN + TAG_LEN || &envelope[0..4] != MAGIC {
        return Err(EnvelopeError::InvalidHeader);
    }
    let version = u16::from_be_bytes(envelope[4..6].try_into().unwrap());
    if version != ENVELOPE_VERSION {
        return Err(EnvelopeError::UnsupportedVersion);
    }
    if envelope[18] != 0 || envelope[19] != 0 {
        return Err(EnvelopeError::InvalidHeader);
    }

    let ciphertext_len_u64 = u64::from_be_bytes(envelope[92..100].try_into().unwrap());
    let ciphertext_len =
        usize::try_from(ciphertext_len_u64).map_err(|_| EnvelopeError::PayloadTooLarge)?;
    if ciphertext_len < TAG_LEN
        || ciphertext_len > MAX_CIPHERTEXT_LEN
        || envelope.len() != HEADER_LEN + ciphertext_len
    {
        return Err(EnvelopeError::InvalidHeader);
    }

    Ok(ParsedHeader {
        metadata: EnvelopeMetadata {
            payload_format: u16::from_be_bytes(envelope[6..8].try_into().unwrap()),
            payload_version: u32::from_be_bytes(envelope[8..12].try_into().unwrap()),
            key_version: u32::from_be_bytes(envelope[12..16].try_into().unwrap()),
            data_kind: DataKind::try_from(envelope[17])?,
        },
        account_kind: envelope[16],
        account_hash: envelope[20..52].try_into().unwrap(),
        salt: envelope[52..68].try_into().unwrap(),
        nonce: envelope[68..92].try_into().unwrap(),
        ciphertext_len,
    })
}

pub(crate) fn encrypt_payload(
    master_key: &[u8; 32],
    key_version: u32,
    account: &AccountId,
    data_kind: DataKind,
    payload_format: u16,
    payload_version: u32,
    plaintext: &[u8],
) -> Result<Vec<u8>, EnvelopeError> {
    let mut salt = [0_u8; SALT_LEN];
    let mut nonce = [0_u8; NONCE_LEN];
    getrandom::fill(&mut salt).map_err(|_| EnvelopeError::RandomUnavailable)?;
    getrandom::fill(&mut nonce).map_err(|_| EnvelopeError::RandomUnavailable)?;
    encrypt_payload_with_randomness(
        master_key,
        key_version,
        account,
        data_kind,
        payload_format,
        payload_version,
        plaintext,
        salt,
        nonce,
    )
}

#[allow(clippy::too_many_arguments)]
fn encrypt_payload_with_randomness(
    master_key: &[u8; 32],
    key_version: u32,
    account: &AccountId,
    data_kind: DataKind,
    payload_format: u16,
    payload_version: u32,
    plaintext: &[u8],
    salt: [u8; SALT_LEN],
    nonce: [u8; NONCE_LEN],
) -> Result<Vec<u8>, EnvelopeError> {
    if plaintext.len() > MAX_CIPHERTEXT_LEN.saturating_sub(TAG_LEN) {
        return Err(EnvelopeError::PayloadTooLarge);
    }
    if matches!(account, AccountId::WindowsSid(sid) if sid.is_empty()) {
        return Err(EnvelopeError::InvalidAccount);
    }

    let metadata = EnvelopeMetadata {
        payload_format,
        payload_version,
        key_version,
        data_kind,
    };
    let header = build_header(metadata, account, &salt, &nonce, plaintext.len() + TAG_LEN)?;
    let key = derive_key(master_key, account, data_kind, key_version, &salt)?;
    let cipher = XChaCha20Poly1305::new(Key::from_slice(key.as_ref()));
    let ciphertext = cipher
        .encrypt(
            XNonce::from_slice(&nonce),
            Payload {
                msg: plaintext,
                aad: &header,
            },
        )
        .map_err(|_| EnvelopeError::AuthenticationFailed)?;

    let mut envelope = Vec::with_capacity(HEADER_LEN + ciphertext.len());
    envelope.extend_from_slice(&header);
    envelope.extend_from_slice(&ciphertext);
    Ok(envelope)
}

pub(crate) fn decrypt_payload(
    master_key: &[u8; 32],
    account: &AccountId,
    expected_data_kind: DataKind,
    envelope: &[u8],
) -> Result<(EnvelopeMetadata, Zeroizing<Vec<u8>>), EnvelopeError> {
    let header = parse_header(envelope)?;
    if header.account_kind != account.kind()
        || header.account_hash != account_hash(account)
        || header.metadata.data_kind != expected_data_kind
    {
        return Err(EnvelopeError::AuthenticationFailed);
    }
    let key = derive_key(
        master_key,
        account,
        expected_data_kind,
        header.metadata.key_version,
        &header.salt,
    )?;
    let cipher = XChaCha20Poly1305::new(Key::from_slice(key.as_ref()));
    let plaintext = cipher
        .decrypt(
            XNonce::from_slice(&header.nonce),
            Payload {
                msg: &envelope[HEADER_LEN..HEADER_LEN + header.ciphertext_len],
                aad: &envelope[..HEADER_LEN],
            },
        )
        .map_err(|_| EnvelopeError::AuthenticationFailed)?;
    Ok((header.metadata, Zeroizing::new(plaintext)))
}

pub(crate) fn profile_payload_format() -> u16 {
    PROFILE_PAYLOAD_FORMAT_JSON
}

#[cfg(test)]
mod tests {
    use super::*;

    const KEY: [u8; 32] = [0x42; 32];
    const SALT: [u8; SALT_LEN] = [0x11; SALT_LEN];
    const NONCE: [u8; NONCE_LEN] = [0x22; NONCE_LEN];

    fn encrypt(account: &AccountId, data_kind: DataKind, plaintext: &[u8]) -> Vec<u8> {
        encrypt_payload_with_randomness(
            &KEY,
            7,
            account,
            data_kind,
            PROFILE_PAYLOAD_FORMAT_JSON,
            1,
            plaintext,
            SALT,
            NONCE,
        )
        .unwrap()
    }

    #[test]
    fn fixed_vector_round_trip() {
        let account = AccountId::LinuxUid(1000);
        let envelope = encrypt(&account, DataKind::Profile, br#"{"version":1}"#);
        assert_eq!(&envelope[0..4], MAGIC);
        assert_eq!(envelope.len(), HEADER_LEN + 29);

        let (metadata, plaintext) =
            decrypt_payload(&KEY, &account, DataKind::Profile, &envelope).unwrap();
        assert_eq!(metadata.key_version, 7);
        assert_eq!(metadata.payload_format, PROFILE_PAYLOAD_FORMAT_JSON);
        assert_eq!(plaintext.as_slice(), br#"{"version":1}"#);
    }

    #[test]
    fn account_and_data_kind_are_bound() {
        let account = AccountId::LinuxUid(1000);
        let envelope = encrypt(&account, DataKind::Profile, b"secret");
        assert_eq!(
            decrypt_payload(
                &KEY,
                &AccountId::LinuxUid(1001),
                DataKind::Profile,
                &envelope
            ),
            Err(EnvelopeError::AuthenticationFailed)
        );
        assert_eq!(
            decrypt_payload(&KEY, &account, DataKind::WindowsPassword, &envelope),
            Err(EnvelopeError::AuthenticationFailed)
        );
    }

    #[test]
    fn tampering_and_truncation_fail_closed() {
        let account = AccountId::WindowsSid("S-1-5-21-1000".to_owned());
        let envelope = encrypt(&account, DataKind::WindowsPassword, b"password");
        for index in [6, 20, 52, 68, HEADER_LEN, envelope.len() - 1] {
            let mut tampered = envelope.clone();
            tampered[index] ^= 1;
            assert!(decrypt_payload(&KEY, &account, DataKind::WindowsPassword, &tampered).is_err());
        }
        assert!(
            decrypt_payload(
                &KEY,
                &account,
                DataKind::WindowsPassword,
                &envelope[..envelope.len() - 1]
            )
            .is_err()
        );
    }

    #[test]
    fn production_encryption_uses_fresh_randomness() {
        let account = AccountId::LinuxUid(1000);
        let first = encrypt_payload(
            &KEY,
            1,
            &account,
            DataKind::Profile,
            PROFILE_PAYLOAD_FORMAT_JSON,
            1,
            b"same",
        )
        .unwrap();
        let second = encrypt_payload(
            &KEY,
            1,
            &account,
            DataKind::Profile,
            PROFILE_PAYLOAD_FORMAT_JSON,
            1,
            b"same",
        )
        .unwrap();
        assert_ne!(first, second);
    }
}
