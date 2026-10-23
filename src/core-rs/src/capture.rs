//! Effects at the store boundary. Only the privileged hosts supply paths/keys.
use crate::SuStatus;
use crate::capture_policy::{
    HARD_BYTE_CAP, MAX_JPEG_BYTES, Outcome, Record, Settings, retention, should_capture, valid_id,
};
use crate::encrypted_store::{DataKind, decrypt_payload, encrypt_payload};
use crate::profile::EncryptedProfileContext;
use crate::storage::atomic_write_private;
use serde::Deserialize;
use std::fs;
use std::path::Path;
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};
use zeroize::Zeroizing;

static STORE_LOCK: Mutex<()> = Mutex::new(());
const MAX_FILE: u64 = MAX_JPEG_BYTES as u64 + 2048;
#[derive(Deserialize)]
#[serde(tag = "op", rename_all = "snake_case", deny_unknown_fields)]
pub enum Command {
    Store {
        settings: Settings,
        reason: Outcome,
        width: u32,
        height: u32,
    },
    List {},
    Fetch {
        id: String,
    },
    Delete {
        id: String,
    },
    DeleteAll {},
    Configure {
        settings: Settings,
    },
    Prune {},
}
fn now() -> Result<u64, SuStatus> {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|t| t.as_secs())
        .map_err(|_| SuStatus::IoError)
}
fn read_bounded(path: &Path) -> Result<Vec<u8>, SuStatus> {
    let metadata = fs::symlink_metadata(path).map_err(|_| SuStatus::IoError)?;
    if !metadata.is_file() || metadata.len() > MAX_FILE {
        return Err(SuStatus::InvalidArgument);
    }
    fs::read(path).map_err(|_| SuStatus::IoError)
}
fn load(
    path: &Path,
    context: &EncryptedProfileContext<'_>,
) -> Result<(Record, Zeroizing<Vec<u8>>), SuStatus> {
    let encrypted = read_bounded(path)?;
    let (meta, payload) = decrypt_payload(
        context.master_key,
        &context.account,
        DataKind::Capture,
        &encrypted,
    )
    .map_err(|_| SuStatus::CryptoError)?;
    if meta.payload_format != 2 || meta.payload_version != 1 {
        return Err(SuStatus::ParseError);
    }
    let split = payload
        .iter()
        .position(|b| *b == b'\n')
        .ok_or(SuStatus::ParseError)?;
    let record: Record =
        serde_json::from_slice(&payload[..split]).map_err(|_| SuStatus::ParseError)?;
    if !record.valid()
        || record.byte_size as usize != payload.len() - split - 1
        || path.file_stem().and_then(|s| s.to_str()) != Some(&record.id)
    {
        return Err(SuStatus::ParseError);
    }
    Ok((record, Zeroizing::new(payload[split + 1..].to_vec())))
}
fn records(dir: &Path, context: &EncryptedProfileContext<'_>) -> Result<Vec<Record>, SuStatus> {
    if !dir.exists() {
        return Ok(Vec::new());
    }
    let mut result = Vec::new();
    for entry in fs::read_dir(dir).map_err(|_| SuStatus::IoError)? {
        let path = entry.map_err(|_| SuStatus::IoError)?.path();
        if path.extension().and_then(|s| s.to_str()) != Some("s2u") {
            continue;
        }
        if result.len() >= 201 {
            return Err(SuStatus::InvalidArgument);
        }
        result.push(load(&path, context)?.0);
    }
    Ok(result)
}
fn policy(dir: &Path) -> Result<Settings, SuStatus> {
    let path = dir.join("retention.json");
    if !path.exists() {
        return Ok(Settings::default());
    }
    let settings: Settings =
        serde_json::from_slice(&read_bounded(&path)?).map_err(|_| SuStatus::ParseError)?;
    if !settings.valid() {
        return Err(SuStatus::InvalidArgument);
    }
    Ok(settings)
}
fn save_policy(dir: &Path, settings: Settings) -> Result<(), SuStatus> {
    if !settings.valid() {
        return Err(SuStatus::InvalidArgument);
    }
    atomic_write_private(
        &dir.join("retention.json"),
        &serde_json::to_vec(&settings).map_err(|_| SuStatus::WriteError)?,
    )
}
fn prune(dir: &Path, context: &EncryptedProfileContext<'_>) -> Result<Vec<Record>, SuStatus> {
    let mut records = records(dir, context)?;
    let deleted = retention(&records, policy(dir)?, now()?, HARD_BYTE_CAP);
    for id in &deleted {
        fs::remove_file(dir.join(format!("{id}.s2u"))).map_err(|_| SuStatus::WriteError)?;
    }
    records.retain(|r| !deleted.contains(&r.id));
    records.sort_by(|a, b| (b.time, &b.id).cmp(&(a.time, &a.id)));
    Ok(records)
}
fn store(
    dir: &Path,
    context: &EncryptedProfileContext<'_>,
    record: &Record,
    jpeg: &[u8],
) -> Result<(), SuStatus> {
    let mut payload = Zeroizing::new(serde_json::to_vec(record).map_err(|_| SuStatus::WriteError)?);
    payload.push(b'\n');
    payload.extend_from_slice(jpeg);
    let encrypted = encrypt_payload(
        context.master_key,
        context.key_version,
        &context.account,
        DataKind::Capture,
        2,
        1,
        &payload,
    )
    .map_err(|_| SuStatus::CryptoError)?;
    atomic_write_private(&dir.join(format!("{}.s2u", record.id)), &encrypted)
}
fn add(
    dir: &Path,
    context: &EncryptedProfileContext<'_>,
    command: Command,
    jpeg: &[u8],
) -> Result<Vec<u8>, SuStatus> {
    let Command::Store {
        settings,
        reason,
        width,
        height,
    } = command
    else {
        unreachable!()
    };
    if !settings.valid()
        || !should_capture(settings, reason)
        || jpeg.len() > MAX_JPEG_BYTES
        || !jpeg.starts_with(&[0xff, 0xd8])
        || !jpeg.ends_with(&[0xff, 0xd9])
    {
        return Err(SuStatus::InvalidArgument);
    }
    let mut random = [0_u8; 16];
    getrandom::fill(&mut random).map_err(|_| SuStatus::CryptoError)?;
    let id: String = random.iter().map(|b| format!("{b:02x}")).collect();
    let record = Record {
        id,
        time: now()?,
        reason,
        width,
        height,
        byte_size: jpeg.len() as u64,
    };
    if !record.valid() {
        return Err(SuStatus::InvalidArgument);
    }
    // The owner can disable capture while a recognition attempt is running.
    // Never let a late result overwrite a newer opt-out or retention policy.
    let current = policy(dir)?;
    if !should_capture(current, reason) {
        return Err(SuStatus::InvalidArgument);
    }
    prune(dir, context)?;
    store(dir, context, &record, jpeg)?;
    prune(dir, context)?;
    serde_json::to_vec(&record).map_err(|_| SuStatus::WriteError)
}

pub fn execute(
    dir: &Path,
    context: &EncryptedProfileContext<'_>,
    command: Command,
    jpeg: &[u8],
) -> Result<Zeroizing<Vec<u8>>, SuStatus> {
    let _lock = STORE_LOCK.lock().map_err(|_| SuStatus::IoError)?;
    // Parent directories belong to the privileged host; reject symlink stores.
    if dir.exists()
        && !fs::symlink_metadata(dir)
            .map_err(|_| SuStatus::IoError)?
            .is_dir()
    {
        return Err(SuStatus::InvalidArgument);
    }
    if matches!(command, Command::Store { .. }) {
        return add(dir, context, command, jpeg).map(Zeroizing::new);
    }
    if let Command::Configure { settings } = command {
        save_policy(dir, settings)?;
    }
    if matches!(command, Command::DeleteAll {}) {
        return delete_all(dir);
    }
    execute_read_or_delete(dir, context, command)
}

fn delete_all(dir: &Path) -> Result<Zeroizing<Vec<u8>>, SuStatus> {
    if dir.exists() {
        for entry in fs::read_dir(dir).map_err(|_| SuStatus::IoError)? {
            let path = entry.map_err(|_| SuStatus::IoError)?.path();
            if path.file_name().and_then(|s| s.to_str()) != Some("retention.json") {
                fs::remove_file(path).map_err(|_| SuStatus::WriteError)?;
            }
        }
    }
    Ok(Zeroizing::new(b"[]".to_vec()))
}
fn execute_read_or_delete(
    dir: &Path,
    context: &EncryptedProfileContext<'_>,
    command: Command,
) -> Result<Zeroizing<Vec<u8>>, SuStatus> {
    let records = prune(dir, context)?;
    match command {
        Command::Fetch { id } | Command::Delete { id } if !valid_id(&id) => {
            Err(SuStatus::InvalidArgument)
        }
        Command::Fetch { id } => {
            if !records.iter().any(|r| r.id == id) {
                return Err(SuStatus::InvalidArgument);
            }
            load(&dir.join(format!("{id}.s2u")), context).map(|(_, jpeg)| jpeg)
        }
        Command::Delete { id } => {
            if !records.iter().any(|r| r.id == id) {
                return Err(SuStatus::InvalidArgument);
            }
            fs::remove_file(dir.join(format!("{id}.s2u"))).map_err(|_| SuStatus::WriteError)?;
            Ok(Zeroizing::new(b"[]".to_vec()))
        }
        _ => serde_json::to_vec(&records)
            .map(Zeroizing::new)
            .map_err(|_| SuStatus::WriteError),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::encrypted_store::AccountId;
    fn context(uid: u32) -> EncryptedProfileContext<'static> {
        EncryptedProfileContext {
            master_key: &[42; 32],
            key_version: 1,
            account: AccountId::LinuxUid(uid),
        }
    }
    fn command() -> Command {
        Command::Store {
            settings: Settings {
                enabled: true,
                max_count: 2,
                ..Settings::default()
            },
            reason: Outcome::Mismatch,
            width: 320,
            height: 240,
        }
    }
    fn enable(dir: &Path, ctx: &EncryptedProfileContext<'_>) {
        execute(
            dir,
            ctx,
            Command::Configure {
                settings: Settings {
                    enabled: true,
                    max_count: 2,
                    ..Settings::default()
                },
            },
            &[],
        )
        .unwrap();
    }
    fn directory() -> std::path::PathBuf {
        let mut random = [0_u8; 8];
        getrandom::fill(&mut random).unwrap();
        std::env::temp_dir().join(format!("s2u-capture-{}", u64::from_le_bytes(random)))
    }
    const JPEG: &[u8] = &[0xff, 0xd8, 1, 2, 3, 0xff, 0xd9];
    #[test]
    fn encrypted_store_roundtrip_isolates_accounts_and_applies_retention() {
        let dir = directory();
        let ctx = context(1000);
        enable(&dir, &ctx);
        let first = execute(&dir, &ctx, command(), JPEG).unwrap();
        let record: Record = serde_json::from_slice(&first).unwrap();
        let path = dir.join(format!("{}.s2u", record.id));
        let ciphertext = fs::read(&path).unwrap();
        assert_eq!(&ciphertext[..4], b"S2UE");
        assert!(!ciphertext.windows(JPEG.len()).any(|b| b == JPEG));
        assert!(!ciphertext.windows(8).any(|b| b == b"mismatch"));
        assert_eq!(
            &*execute(
                &dir,
                &ctx,
                Command::Fetch {
                    id: record.id.clone()
                },
                &[]
            )
            .unwrap(),
            JPEG
        );
        assert_eq!(
            execute(&dir, &context(1001), Command::List {}, &[]),
            Err(SuStatus::CryptoError)
        );
        let wrong_key = EncryptedProfileContext {
            master_key: &[43; 32],
            ..context(1000)
        };
        assert_eq!(
            execute(&dir, &wrong_key, Command::List {}, &[]),
            Err(SuStatus::CryptoError)
        );
        execute(&dir, &ctx, command(), JPEG).unwrap();
        execute(&dir, &ctx, command(), JPEG).unwrap();
        let list = execute(&dir, &ctx, Command::List {}, &[]).unwrap();
        assert_eq!(
            serde_json::from_slice::<Vec<Record>>(&list).unwrap().len(),
            2
        );
        execute(&dir, &ctx, Command::DeleteAll {}, &[]).unwrap();
        assert!(records(&dir, &ctx).unwrap().is_empty());
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn expired_tampered_and_unsafe_records_fail_closed() {
        let dir = directory();
        let ctx = context(1000);
        enable(&dir, &ctx);
        execute(&dir, &ctx, command(), JPEG).unwrap();
        let mut record = records(&dir, &ctx).unwrap().remove(0);
        record.time = 1;
        store(&dir, &ctx, &record, JPEG).unwrap();
        assert_eq!(
            &*execute(&dir, &ctx, Command::Prune {}, &[]).unwrap(),
            b"[]"
        );
        execute(&dir, &ctx, command(), JPEG).unwrap();
        let record = records(&dir, &ctx).unwrap().remove(0);
        let path = dir.join(format!("{}.s2u", record.id));
        let mut bytes = fs::read(&path).unwrap();
        *bytes.last_mut().unwrap() ^= 1;
        fs::write(&path, bytes).unwrap();
        assert_eq!(
            execute(&dir, &ctx, Command::List {}, &[]),
            Err(SuStatus::CryptoError)
        );
        execute(&dir, &ctx, Command::DeleteAll {}, &[]).unwrap();
        assert!(
            execute(
                &dir,
                &ctx,
                Command::Fetch {
                    id: "../profiles".into()
                },
                &[]
            )
            .is_err()
        );
    }
    #[test]
    fn off_no_face_and_oversize_never_write() {
        let dir = directory();
        for (settings, reason) in [
            (Settings::default(), Outcome::Mismatch),
            (
                Settings {
                    enabled: true,
                    ..Settings::default()
                },
                Outcome::NoFace,
            ),
        ] {
            let command = Command::Store {
                settings,
                reason,
                width: 320,
                height: 240,
            };
            assert_eq!(
                execute(&dir, &context(1000), command, JPEG),
                Err(SuStatus::InvalidArgument)
            );
            assert!(!dir.exists());
        }
        assert_eq!(
            execute(
                &dir,
                &context(1000),
                command(),
                &vec![1; MAX_JPEG_BYTES + 1]
            ),
            Err(SuStatus::InvalidArgument)
        );
        assert!(!dir.exists());
    }
    #[test]
    fn delete_one_and_all_remove_images_and_interrupted_write_artifacts() {
        let dir = directory();
        let ctx = context(1000);
        enable(&dir, &ctx);
        let bytes = execute(&dir, &ctx, command(), JPEG).unwrap();
        let record: Record = serde_json::from_slice(&bytes).unwrap();
        execute(
            &dir,
            &ctx,
            Command::Delete {
                id: record.id.clone(),
            },
            &[],
        )
        .unwrap();
        assert!(execute(&dir, &ctx, Command::Fetch { id: record.id }, &[]).is_err());
        fs::write(dir.join(".interrupted.s2u.tmp-1"), b"encrypted orphan").unwrap();
        execute(&dir, &ctx, Command::DeleteAll {}, &[]).unwrap();
        assert_eq!(fs::read_dir(&dir).unwrap().count(), 1);
        assert!(policy(&dir).unwrap().enabled);
        fs::remove_dir_all(dir).unwrap();
    }
    #[cfg(unix)]
    #[test]
    fn private_files_and_symlink_rejection() {
        use std::os::unix::fs::{PermissionsExt, symlink};
        let dir = directory();
        execute(
            &dir,
            &context(1000),
            Command::Configure {
                settings: Settings {
                    enabled: true,
                    ..Settings::default()
                },
            },
            &[],
        )
        .unwrap();
        execute(&dir, &context(1000), command(), JPEG).unwrap();
        let record = records(&dir, &context(1000)).unwrap().remove(0);
        let file = dir.join(format!("{}.s2u", record.id));
        assert_eq!(
            fs::metadata(&file).unwrap().permissions().mode() & 0o777,
            0o600
        );
        assert_eq!(
            fs::metadata(&dir).unwrap().permissions().mode() & 0o777,
            0o700
        );
        let link = dir.with_extension("link");
        symlink(&dir, &link).unwrap();
        assert_eq!(
            execute(&link, &context(1000), Command::List {}, &[]),
            Err(SuStatus::InvalidArgument)
        );
        fs::remove_file(link).unwrap();
        fs::remove_dir_all(dir).unwrap();
    }
}

#[cfg(test)]
mod opt_out_tests {
    use super::*;
    #[test]
    fn late_results_cannot_override_an_owner_opt_out() {
        let mut random = [0; 8];
        getrandom::fill(&mut random).unwrap();
        let dir = std::env::temp_dir().join(format!("s2u-opt-out-{}", u64::from_le_bytes(random)));
        let context = EncryptedProfileContext {
            master_key: &[42; 32],
            key_version: 1,
            account: crate::encrypted_store::AccountId::LinuxUid(1000),
        };
        execute(
            &dir,
            &context,
            Command::Configure {
                settings: Settings::default(),
            },
            &[],
        )
        .unwrap();
        let late = Command::Store {
            settings: Settings {
                enabled: true,
                ..Settings::default()
            },
            reason: Outcome::Mismatch,
            width: 320,
            height: 240,
        };
        assert_eq!(
            execute(&dir, &context, late, &[0xff, 0xd8, 0xff, 0xd9]),
            Err(SuStatus::InvalidArgument)
        );
        assert!(!policy(&dir).unwrap().enabled);
        assert!(records(&dir, &context).unwrap().is_empty());
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn capture_commands_reject_unknown_fields_and_operations() {
        for json in [
            r#"{"op":"list","unexpected":true}"#,
            r#"{"op":"prune","unexpected":true}"#,
            r#"{"op":"delete_all","unexpected":true}"#,
            r#"{"op":"export_plaintext"}"#,
        ] {
            assert!(serde_json::from_str::<Command>(json).is_err());
        }
    }
}
