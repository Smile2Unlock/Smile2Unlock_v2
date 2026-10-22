//! Pure logging format shared in behavior with common/log/core.cpp.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Level {
    Debug,
    Info,
    Warn,
    Error,
}
impl Level {
    pub fn parse(value: &str) -> Option<Self> {
        match value.to_ascii_lowercase().as_str() {
            "debug" => Some(Self::Debug),
            "info" => Some(Self::Info),
            "warn" => Some(Self::Warn),
            "error" => Some(Self::Error),
            _ => None,
        }
    }
    fn name(self) -> &'static str {
        match self {
            Self::Debug => "DEBUG",
            Self::Info => "INFO",
            Self::Warn => "WARN",
            Self::Error => "ERROR",
        }
    }
}
fn clean(value: &[u8]) -> String {
    let mut text = String::new();
    let mut index = 0;
    while index < value.len() {
        let width = match value[index] {
            0..=0x7f => 1,
            0xc2..=0xdf => 2,
            0xe0..=0xef => 3,
            0xf0..=0xf4 => 4,
            _ => 0,
        };
        let part = value
            .get(index..index + width)
            .filter(|_| width > 0)
            .and_then(|bytes| std::str::from_utf8(bytes).ok());
        text.push_str(part.unwrap_or("�"));
        index += if part.is_some() { width } else { 1 };
    }
    if text.len() <= 512 {
        return text;
    }
    let marker = "…[truncated]";
    let mut end = 512 - marker.len();
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    format!("{}{marker}", &text[..end])
}
fn quote(value: &[u8]) -> String {
    let text = clean(value);
    let quoted = text.is_empty()
        || text
            .bytes()
            .any(|c| c <= 0x20 || c == 0x7f || b"=\"\\".contains(&c));
    let mut output = String::new();
    if quoted {
        output.push('"');
    }
    for c in text.chars() {
        match c {
            '\\' => output.push_str("\\\\"),
            '"' => output.push_str("\\\""),
            '\n' => output.push_str("\\n"),
            '\r' => output.push_str("\\r"),
            '\t' => output.push_str("\\t"),
            c if (c as u32) < 0x20 || c == '\x7f' => {
                output.push_str(&format!("\\x{:02x}", c as u32))
            }
            c => output.push(c),
        }
    }
    if quoted {
        output.push('"');
    }
    output
}
fn identifier(value: &str) -> String {
    let result: String = clean(value.as_bytes())
        .bytes()
        .map(|c| {
            if c.is_ascii_alphanumeric() || b"_-.".contains(&c) {
                c as char
            } else {
                '_'
            }
        })
        .collect();
    if result.is_empty() {
        "_".into()
    } else {
        result
    }
}
fn sensitive(key: &str) -> bool {
    let name = key.to_ascii_lowercase();
    [
        "password",
        "passwd",
        "secret",
        "token",
        "nonce",
        "feature",
        "embedding",
        "key",
        "credential",
    ]
    .iter()
    .any(|word| name.contains(word))
}
// Gregorian civil date from days since 1970-01-01, with Euclidean divisions for negative dates.
fn civil_date(days: i64) -> (i64, i64, i64) {
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let year = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = mp + if mp < 10 { 3 } else { -9 };
    (year + i64::from(month <= 2), month, day)
}
fn timestamp(millis: i64, offset: i32) -> String {
    let offset = offset.clamp(-1439, 1439);
    let local = millis + i64::from(offset) * 60000;
    let (year, month, day) = civil_date(local.div_euclid(86400000));
    let time = local.rem_euclid(86400000);
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}.{:03}{}{:02}:{:02}",
        time / 3600000,
        time / 60000 % 60,
        time / 1000 % 60,
        time % 1000,
        if offset < 0 { '-' } else { '+' },
        offset.abs() / 60,
        offset.abs() % 60
    )
}
pub struct Record<'a> {
    pub millis: i64,
    pub offset: i32,
    pub level: Level,
    pub component: &'a str,
    pub pid: u32,
    pub tid: u32,
    pub event: &'a str,
    pub fields: &'a [(&'a str, &'a [u8])],
}
pub fn format_record(record: &Record<'_>) -> String {
    let mut output = format!(
        "{} {} {} {}:{} event={}",
        timestamp(record.millis, record.offset),
        record.level.name(),
        identifier(record.component),
        record.pid,
        record.tid,
        quote(record.event.as_bytes())
    );
    for (key, value) in record.fields {
        let value = if sensitive(key) {
            b"[redacted]"
        } else {
            *value
        };
        output.push_str(&format!(" {}={}", identifier(key), quote(value)));
    }
    output.push('\n');
    output
}

#[cfg(test)]
mod tests {
    use super::*;
    fn decode(value: &str) -> Vec<u8> {
        let mut bytes = value.bytes();
        let mut result = Vec::new();
        while let Some(c) = bytes.next() {
            if c != b'\\' {
                result.push(c);
                continue;
            }
            match bytes.next() {
                Some(b'n') => result.push(b'\n'),
                Some(b'r') => result.push(b'\r'),
                Some(b't') => result.push(b'\t'),
                Some(b'x') => {
                    let hi = bytes
                        .next()
                        .and_then(|c| (c as char).to_digit(16))
                        .expect("golden hex");
                    let lo = bytes
                        .next()
                        .and_then(|c| (c as char).to_digit(16))
                        .expect("golden hex");
                    result.push((hi * 16 + lo) as u8);
                }
                Some(c) => result.push(c),
                None => panic!("invalid golden escape"),
            }
        }
        result
    }
    #[test]
    fn shared_golden() {
        let golden = include_str!("../../../../../tests/log/format_golden.txt");
        let mut count = 0;
        for line in golden
            .lines()
            .filter(|line| !line.is_empty() && !line.starts_with('#'))
        {
            let parts: Vec<_> = line.split('|').collect();
            assert_eq!(parts.len(), 10);
            let value = decode(parts[8]);
            let record = Record {
                millis: parts[0].parse().unwrap(),
                offset: parts[1].parse().unwrap(),
                level: Level::parse(parts[2]).unwrap(),
                component: parts[3],
                pid: parts[4].parse().unwrap(),
                tid: parts[5].parse().unwrap(),
                event: parts[6],
                fields: &[(parts[7], &value)],
            };
            assert_eq!(format_record(&record).as_bytes(), decode(parts[9]));
            count += 1;
        }
        assert!(count >= 7);
    }
    #[test]
    fn limits_and_levels() {
        assert!(Level::parse("invalid").is_none());
        assert!(Level::parse("DeBuG") == Some(Level::Debug));
        assert_eq!(clean(&vec![b'a'; 600]).len(), 512);
        assert!(clean(&vec![0xff; 512]).len() <= 512);
        for key in [
            "PASSWORD",
            "passwd",
            "secret",
            "token",
            "nonce",
            "feature",
            "embedding",
            "key",
            "credential",
        ] {
            assert!(sensitive(key));
        }
    }
}
