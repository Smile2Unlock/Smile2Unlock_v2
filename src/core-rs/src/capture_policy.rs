//! Pure capture decisions and wire metadata. Times are UTC Unix seconds.
use serde::{Deserialize, Serialize};

pub const MAX_JPEG_BYTES: usize = 45 * 1024;
pub const HARD_BYTE_CAP: u64 = 20 * 1024 * 1024;
pub const LONG_EDGE: u32 = 320;

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Outcome {
    Mismatch,
    LivenessRejected,
    NoFace,
    Accepted,
    Unavailable,
    Cancelled,
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
pub struct Settings {
    pub enabled: bool,
    pub days: u32,
    pub max_count: u32,
}
impl Default for Settings {
    fn default() -> Self {
        Self {
            enabled: false,
            days: 7,
            max_count: 50,
        }
    }
}
impl Settings {
    pub fn valid(self) -> bool {
        (1..=90).contains(&self.days) && (1..=200).contains(&self.max_count)
    }
}
pub fn should_capture(settings: Settings, outcome: Outcome) -> bool {
    settings.enabled && matches!(outcome, Outcome::Mismatch | Outcome::LivenessRejected)
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Record {
    pub id: String,
    pub time: u64,
    pub reason: Outcome,
    pub width: u32,
    pub height: u32,
    pub byte_size: u64,
}
impl Record {
    pub fn valid(&self) -> bool {
        valid_id(&self.id)
            && matches!(self.reason, Outcome::Mismatch | Outcome::LivenessRejected)
            && dimensions(self.width, self.height) == Some((self.width, self.height))
            && (1..=MAX_JPEG_BYTES as u64).contains(&self.byte_size)
    }
}
pub fn valid_id(id: &str) -> bool {
    id.len() == 32
        && id
            .bytes()
            .all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase())
}
pub fn dimensions(width: u32, height: u32) -> Option<(u32, u32)> {
    if width == 0 || height == 0 {
        return None;
    }
    let edge = width.max(height);
    if edge <= LONG_EDGE {
        return Some((width, height));
    }
    Some((
        (u64::from(width) * u64::from(LONG_EDGE) / u64::from(edge)).max(1) as u32,
        (u64::from(height) * u64::from(LONG_EDGE) / u64::from(edge)).max(1) as u32,
    ))
}

pub fn retention(records: &[Record], settings: Settings, now: u64, byte_cap: u64) -> Vec<String> {
    let mut ordered: Vec<_> = records.iter().collect();
    ordered.sort_by(|a, b| (a.time, &a.id).cmp(&(b.time, &b.id)));
    let mut count = ordered.len();
    let mut bytes = ordered
        .iter()
        .map(|r| r.byte_size)
        .fold(0_u64, u64::saturating_add);
    let mut deleted = Vec::new();
    for record in ordered {
        if now.saturating_sub(record.time) >= u64::from(settings.days) * 86400
            || count > settings.max_count as usize
            || bytes > byte_cap
        {
            count -= 1;
            bytes = bytes.saturating_sub(record.byte_size);
            deleted.push(record.id.clone());
        }
    }
    deleted
}

#[cfg(test)]
mod tests {
    use super::*;
    fn record(time: u64) -> Record {
        Record {
            id: format!("{time:032x}"),
            time,
            reason: Outcome::Mismatch,
            width: 320,
            height: 240,
            byte_size: 100,
        }
    }
    #[test]
    fn opt_in_and_detected_rejections_only() {
        for outcome in [
            Outcome::Mismatch,
            Outcome::LivenessRejected,
            Outcome::NoFace,
            Outcome::Accepted,
            Outcome::Unavailable,
            Outcome::Cancelled,
        ] {
            assert!(!should_capture(Settings::default(), outcome));
            assert_eq!(
                should_capture(
                    Settings {
                        enabled: true,
                        ..Settings::default()
                    },
                    outcome
                ),
                matches!(outcome, Outcome::Mismatch | Outcome::LivenessRejected)
            );
        }
    }
    #[test]
    fn ranges_and_dimensions() {
        for days in [0, 91, u32::MAX] {
            assert!(
                !Settings {
                    days,
                    ..Settings::default()
                }
                .valid()
            );
        }
        for max_count in [0, 201, u32::MAX] {
            assert!(
                !Settings {
                    max_count,
                    ..Settings::default()
                }
                .valid()
            );
        }
        assert!(
            Settings {
                days: 90,
                max_count: 200,
                enabled: true
            }
            .valid()
        );
        assert_eq!(dimensions(1920, 1080), Some((320, 180)));
        assert_eq!(dimensions(1080, 1920), Some((180, 320)));
        assert_eq!(dimensions(u32::MAX, 1), Some((320, 1)));
        assert_eq!(dimensions(0, 20), None);
        assert_eq!(dimensions(30, 20), Some((30, 20)));
    }
    #[test]
    fn retention_expiry_count_bytes_and_clock_rollback() {
        let records = [record(30), record(10), record(20)];
        let policy = Settings {
            max_count: 2,
            ..Settings::default()
        };
        assert_eq!(
            retention(&records, policy, 30, HARD_BYTE_CAP),
            [format!("{:032x}", 10)]
        );
        assert_eq!(
            retention(&records, policy, 30, 100),
            [format!("{:032x}", 10), format!("{:032x}", 20)]
        );
        assert_eq!(
            retention(&records, policy, 7 * 86400 + 30, HARD_BYTE_CAP).len(),
            3
        );
        assert!(retention(&[record(30)], policy, 0, HARD_BYTE_CAP).is_empty());
        assert!(retention(&[], policy, u64::MAX, 0).is_empty());
    }
    #[test]
    fn metadata_roundtrip_and_path_validation() {
        let record = record(123);
        assert!(record.valid());
        assert_eq!(
            serde_json::from_slice::<Record>(&serde_json::to_vec(&record).unwrap()).unwrap(),
            record
        );
        for id in ["../profiles.s2u", "", "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"] {
            assert!(!valid_id(id));
        }
    }
}
