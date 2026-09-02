//! Minimal file logger for diagnosing LogonUI integration (Phase 4).
//!
//! Writes to `%TEMP%\su_cp.log` (SYSTEM-writable) with an appended timestamp.
//! Every call is fail-silent: logging must never break the COM surface.
//! This module is diagnostic only and is not part of the production surface.

use std::io::Write;
use std::time::{SystemTime, UNIX_EPOCH};

#[cfg(windows)]
const LOG_PATH: &str = "C:\\Windows\\Temp\\su_cp.log";

/// Append one line to the diagnostic log. Never panics, never unwraps.
pub fn cp_log(msg: &str) {
    let millis = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0);
    let mut line = String::with_capacity(msg.len() + 32);
    line.push_str(&millis.to_string());
    line.push_str(" ");
    line.push_str(msg);
    line.push('\n');

    #[cfg(windows)]
    {
        if let Ok(mut f) = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(LOG_PATH)
        {
            let _ = f.write_all(line.as_bytes());
            let _ = f.flush();
        }
    }
    #[cfg(not(windows))]
    {
        let _ = line;
    }
}
