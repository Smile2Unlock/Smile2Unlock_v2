//! Bounded, level-filtered, fail-silent diagnostics for the thin COM adapter.
#[path = "log_format.rs"]
mod format;
#[cfg(windows)]
#[path = "log_windows.rs"]
mod platform;
use format::{Level, Record, format_record};
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};
use std::time::{SystemTime, UNIX_EPOCH};

struct State {
    level: Level,
    invalid: bool,
    path: Option<PathBuf>,
    writer: Mutex<bool>,
}
fn state() -> &'static State {
    static STATE: OnceLock<State> = OnceLock::new();
    STATE.get_or_init(|| {
        #[cfg(windows)]
        let registry = platform::registry_level();
        #[cfg(not(windows))]
        let registry = None::<String>;
        let setting = std::env::var("SMILE2UNLOCK_LOG_LEVEL").ok().or(registry);
        let parsed = setting.as_deref().and_then(Level::parse);
        #[cfg(windows)]
        let path = platform::path();
        #[cfg(not(windows))]
        let path = None;
        State {
            level: parsed.unwrap_or(Level::Info),
            invalid: setting.is_some() && parsed.is_none(),
            path,
            writer: Mutex::new(false),
        }
    })
}
fn emit(state: &State, level: Level, event: &str, msg: &str) {
    let millis = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0);
    #[cfg(windows)]
    let (tid, offset) = platform::thread_and_offset();
    #[cfg(not(windows))]
    let (tid, offset) = (0, 0);
    let line = format_record(&Record {
        millis,
        offset,
        level,
        component: "credential-provider",
        pid: std::process::id(),
        tid,
        event,
        fields: &[("detail", msg.as_bytes())],
    });
    #[cfg(windows)]
    if state
        .path
        .as_ref()
        .and_then(|p| platform::append(p, &line))
        .is_none()
    {
        platform::debug_output(&line);
    }
    #[cfg(not(windows))]
    let _ = (state, line);
}
fn log(level: Level, msg: &str) {
    let state = state();
    // A poisoned diagnostic mutex must not panic inside LogonUI.
    let Ok(mut warned) = state.writer.lock() else {
        return;
    };
    if state.invalid && !*warned {
        emit(state, Level::Warn, "log.level_invalid", "using info");
        *warned = true;
    }
    if level >= state.level {
        emit(state, level, "cp.trace", msg);
    }
}
pub fn cp_log(msg: &str) {
    log(Level::Info, msg);
}
pub fn cp_debug(msg: &str) {
    log(Level::Debug, msg);
}
pub fn cp_warn(msg: &str) {
    log(Level::Warn, msg);
}
pub fn cp_error(msg: &str) {
    log(Level::Error, msg);
}
