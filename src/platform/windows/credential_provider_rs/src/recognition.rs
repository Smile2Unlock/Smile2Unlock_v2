//! Face-recognition client (UDP) and trigger state machine.
//!
//! Talks to the Smile2Unlock app recognition service (su_app.exe):
//! - request: UDP 127.0.0.1:51236, `UdpAuthRequestPacket` (magic "AUTH")
//! - status:  UDP 127.0.0.1:51234, `UdpStatusPacket` (magic 0x8581DAF3)
//!
//! Protocol mirrors the C++ baseline packet definitions
//! (udp_auth_request_packet.h / udp_status_packet.h, archived). The provider is a pure
//! UDP client: it never runs a camera or recognizer inside LogonUI.
//!
//! Transport security (user requirement):
//! - Loopback only (127.0.0.1): the sockets never leave the machine.
//! - Status packets are accepted only when they carry the session_id of the
//!   most recent request this provider sent (no cross-session/random
//!   injection), and only inside a freshness window (no replay of old
//!   packets). A forged-but-session-matched packet still requires local
//!   administrator-level packet inspection; this is the same trust level as
//!   the C++ baseline. Full forgery resistance would need su_app-side
//!   signing and is tracked as a hardening item.
//! - Face success is only ever used as a GATE to fetch the stored secret
//!   through the LocalSystem pipe; the pipe itself never trusts UDP input.
//!
//! Trigger modes (registry HKLM\SOFTWARE\Smile2Unlock\Recognition):
//! - Manual  (RecognitionMode=0, default): the password-box Enter path
//!   (GetSerialization) sends START_RECOGNITION; success auto-submits,
//!   failure rejects the submit.
//! - Auto    (RecognitionMode=1): a worker waits AutoDelaySec after the
//!   lock screen appears, then triggers; sleep/hibernate/lid suspension
//!   pauses the countdown (GetTickCount64 stops advancing), and resume
//!   triggers immediately. Failures retry every RetryDelaySec until success
//!   or the session ends.

use std::net::{Ipv4Addr, UdpSocket};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use windows::Win32::System::Registry::{
    RegCloseKey, RegOpenKeyExW, RegQueryValueExW, HKEY, HKEY_LOCAL_MACHINE, REG_DWORD,
    REG_SAM_FLAGS,
};

use crate::log::cp_log;

// ---------------------------------------------------------------------------
// Wire protocol (C++ parity)
// ---------------------------------------------------------------------------

pub const AUTH_REQUEST_MAGIC: u32 = 0x4155_5448; // "AUTH"
pub const AUTH_REQUEST_VERSION: u32 = 1;
pub const STATUS_MAGIC: u32 = 0x8581_DAF3;
pub const STATUS_VERSION: u32 = 2;
pub const CP_STATUS_PORT: u16 = 51234;
pub const AUTH_REQUEST_PORT: u16 = 51236;
pub const UDP_STATUS_MAX_FEATURE_BYTES: usize = 48 * 1024;
/// Status packets older than this (wall clock, ms) are treated as replays.
pub const STATUS_FRESHNESS_MS: u64 = 30_000;

/// AuthRequestType (C++ baseline udp_auth_request_packet.h).
pub const REQ_START_RECOGNITION: i32 = 1;
pub const REQ_CANCEL_RECOGNITION: i32 = 2;
pub const REQ_QUERY_STATUS: i32 = 3;

/// RecognitionStatus (C++ baseline recognition_status.h).
pub const RS_IDLE: i32 = 0;
pub const RS_RECOGNIZING: i32 = 1;
pub const RS_SUCCESS: i32 = 2;
pub const RS_FAILED: i32 = 3;
pub const RS_TIMEOUT: i32 = 4;
pub const RS_RECOGNITION_ERROR: i32 = 5;
pub const RS_FACE_DETECTED: i32 = 6;
pub const RS_PROCESS_ENDED: i32 = 7;

#[repr(C, packed)]
struct UdpAuthRequestPacket {
    magic_number: u32,
    version: u32,
    request_type: i32,
    username_hint: [u8; 64],
    timestamp: u64,
    session_id: u32,
}

#[repr(C, packed)]
struct UdpStatusPacket {
    magic_number: u32,
    version: u32,
    status_code: i32,
    session_id: u32,
    feature_bytes: u32,
    username: [u8; 64],
    feature: [u8; UDP_STATUS_MAX_FEATURE_BYTES],
    timestamp: u64,
}

const CONFIG_KEY: &str = r"SOFTWARE\Smile2Unlock\Recognition";

/// Trigger/retry configuration, loaded from the registry with defaults.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RecognitionConfig {
    /// 0 = manual (password-box Enter triggers), 1 = auto (lock-screen delay).
    pub mode: u32,
    /// Auto mode: seconds after the lock screen appears before triggering.
    pub auto_delay_sec: u32,
    /// Auto mode: seconds between failed attempts.
    pub retry_delay_sec: u32,
    /// Per-attempt wait for a terminal status.
    pub timeout_sec: u32,
}

impl Default for RecognitionConfig {
    fn default() -> Self {
        Self {
            mode: 0,
            auto_delay_sec: 3,
            retry_delay_sec: 5,
            timeout_sec: 30,
        }
    }
}

fn read_dword(key: HKEY, name: &[u16], fallback: u32) -> u32 {
    let mut value: u32 = 0;
    let mut size = core::mem::size_of::<u32>() as u32;
    let mut value_type = REG_DWORD;
    let status = unsafe {
        RegQueryValueExW(
            key,
            windows_core::PCWSTR(name.as_ptr()),
            None,
            Some(&mut value_type),
            Some(&mut value as *mut u32 as *mut u8),
            Some(&mut size),
        )
    };
    if status.is_ok() && value_type == REG_DWORD && size >= 4 {
        value
    } else {
        fallback
    }
}

/// Load the recognition configuration from HKLM. Fails soft (defaults) so a
/// missing key can never break the logon surface.
pub fn load_config() -> RecognitionConfig {
    let mut cfg = RecognitionConfig::default();
    let mut key: HKEY = HKEY::default();
    let wide_key: Vec<u16> = CONFIG_KEY.encode_utf16().chain(core::iter::once(0)).collect();
    let status = unsafe {
        RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            windows_core::PCWSTR(wide_key.as_ptr()),
            None,
            REG_SAM_FLAGS(0x20019), // KEY_READ
            &mut key,
        )
    };
    if status.is_err() {
        cp_log("recognition: config key missing, using defaults");
        return cfg;
    }
    let name_mode = "RecognitionMode".encode_utf16().chain(core::iter::once(0)).collect::<Vec<u16>>();
    let name_delay = "AutoDelaySec".encode_utf16().chain(core::iter::once(0)).collect::<Vec<u16>>();
    let name_retry = "RetryDelaySec".encode_utf16().chain(core::iter::once(0)).collect::<Vec<u16>>();
    let name_timeout = "TimeoutSec".encode_utf16().chain(core::iter::once(0)).collect::<Vec<u16>>();
    cfg.mode = read_dword(key, &name_mode, cfg.mode);
    cfg.auto_delay_sec = read_dword(key, &name_delay, cfg.auto_delay_sec);
    cfg.retry_delay_sec = read_dword(key, &name_retry, cfg.retry_delay_sec);
    cfg.timeout_sec = read_dword(key, &name_timeout, cfg.timeout_sec);
    unsafe { RegCloseKey(key) };
    cp_log(&format!(
        "recognition: config mode={} delay={} retry={} timeout={}",
        cfg.mode, cfg.auto_delay_sec, cfg.retry_delay_sec, cfg.timeout_sec
    ));
    cfg
}

/// Shared state between the object, the status receiver and the worker.
struct Shared {
    /// Latest status accepted from the app (RS_*).
    status: Mutex<i32>,
    cv: Condvar,
    /// session_id of the most recent request this provider sent. Status
    /// packets with a different session are ignored (anti-injection).
    latest_session: AtomicU32,
    /// Face auth succeeded: GetSerialization may fetch the stored secret.
    face_ready: AtomicBool,
    /// Set by GetCredentialCount when it consumed face_ready for the
    /// auto-logon path; GetSerialization consumes it to skip the manual
    /// gate for the LogonUI auto-submit call.
    auto_grant: AtomicBool,
    /// Incremented by the worker before each START_RECOGNITION send.
    /// GetSerialization waits for an attempt newer than the one in flight
    /// when the user pressed Enter, so a stale status cannot satisfy the
    /// manual gate.
    attempt_seq: AtomicU32,
    /// A manual trigger is pending (password-box Enter in flight).
    manual_pending: AtomicBool,
    /// Fired on the worker thread when recognition succeeds (provider:
    /// CredentialsChanged via GIT).
    on_success: Mutex<Option<Box<dyn Fn() + Send>>>,
    stop: AtomicBool,
}

/// The recognition client owned by the provider.
pub struct Recognition {
    config: RecognitionConfig,
    shared: Arc<Shared>,
    rx_thread: Mutex<Option<JoinHandle<()>>>,
    worker: Mutex<Option<JoinHandle<()>>>,
    /// Status socket bound to 51234 (None if the port was taken -> degraded,
    /// recognition unavailable, password flow still works).
    status_socket_ok: bool,
}

impl Recognition {
    /// Bind the status socket and start the receiver thread. The worker
    /// (auto timer / manual waiter) is started by `start_auto`/`start_manual`.
    pub fn new(config: RecognitionConfig) -> Self {
        let shared = Arc::new(Shared {
            status: Mutex::new(RS_IDLE),
            cv: Condvar::new(),
            latest_session: AtomicU32::new(0),
            face_ready: AtomicBool::new(false),
            auto_grant: AtomicBool::new(false),
            attempt_seq: AtomicU32::new(0),
            manual_pending: AtomicBool::new(false),
            on_success: Mutex::new(None),
            stop: AtomicBool::new(false),
        });
        let mut rec = Self {
            config,
            shared,
            rx_thread: Mutex::new(None),
            worker: Mutex::new(None),
            status_socket_ok: false,
        };
        if let Ok(socket) = UdpSocket::bind((Ipv4Addr::LOCALHOST, CP_STATUS_PORT)) {
            rec.status_socket_ok = true;
            let shared = Arc::clone(&rec.shared);
            *rec.rx_thread.lock().unwrap() =
                Some(std::thread::spawn(move || recv_loop(socket, shared)));
            cp_log(&format!("recognition: status receiver on :{}", CP_STATUS_PORT));
        } else {
            cp_log(&format!(
                "recognition: status socket bind failed - recognition degraded"
            ));
        }
        rec
    }

    /// Whether the status channel is usable (port not taken).
    pub fn available(&self) -> bool {
        self.status_socket_ok
    }

    /// Install the success callback (provider: CredentialsChanged via GIT).
    /// Fired on the worker thread when recognition succeeds.
    pub fn set_on_success(&self, f: Box<dyn Fn() + Send>) {
        *self.shared.on_success.lock().unwrap() = Some(f);
    }

    /// Start the worker (the single loop serves both modes; the mode comes
    /// from the config).
    pub fn start_auto(&self) {
        self.start_worker("auto");
    }

    /// Start the worker in manual mode.
    pub fn start_manual(&self) {
        self.start_worker("manual");
    }

    fn start_worker(&self, label: &str) {
        let mut slot = self.worker.lock().unwrap();
        if slot.is_some() || !self.status_socket_ok {
            return;
        }
        let cfg = self.config;
        let shared = Arc::clone(&self.shared);
        *slot = Some(std::thread::spawn(move || {
            worker_loop(cfg, shared);
        }));
        cp_log(&format!("recognition: {} worker started", label));
    }

    /// Manual mode, password-box Enter path: arm the manual worker and send
    /// START_RECOGNITION. No-op when an attempt is already in flight.
    pub fn trigger_manual(&self) {
        if self.shared.manual_pending.load(Ordering::Acquire) {
            cp_log("recognition: manual trigger skipped (already pending)");
            return;
        }
        self.shared.manual_pending.store(true, Ordering::Release);
        self.shared.cv.notify_all();
    }

    /// Whether face auth succeeded.
    pub fn is_ready(&self) -> bool {
        self.shared.face_ready.load(Ordering::Acquire)
    }

    /// Consume the ready flag once (GetCredentialCount auto-logon).
    pub fn consume_ready(&self) -> bool {
        self.shared.face_ready.swap(false, Ordering::AcqRel)
    }

    /// GetCredentialCount consumed face_ready for the auto-logon path;
    /// LogonUI's follow-up GetSerialization must skip the manual gate.
    pub fn mark_auto_grant(&self) {
        self.shared.auto_grant.store(true, Ordering::Release);
    }

    /// Consume the auto-logon grant (one GetSerialization call).
    pub fn consume_auto_grant(&self) -> bool {
        self.shared.auto_grant.swap(false, Ordering::AcqRel)
    }

    /// Sequence number of the most recent START_RECOGNITION send.
    pub fn attempt_seq(&self) -> u32 {
        self.shared.attempt_seq.load(Ordering::Acquire)
    }

    /// Manual face gate for GetSerialization: arm the worker, then wait for
    /// the outcome of the attempt it runs. Returns the terminal RS_* status
    /// (RS_SUCCESS on success, RS_TIMEOUT/RS_FAILED/... on failure).
    pub fn trigger_and_wait(&self) -> i32 {
        let seq0 = self.attempt_seq();
        self.trigger_manual();
        self.wait_outcome(seq0, Duration::from_secs(self.config.timeout_sec as u64))
    }

    /// Wait until an attempt newer than `seq0` reaches a terminal status, or
    /// face_ready becomes set (auto worker succeeded meanwhile), or the
    /// timeout/stop fires.
    fn wait_outcome(&self, seq0: u32, timeout: Duration) -> i32 {
        let deadline = std::time::Instant::now() + timeout;
        let mut guard = self.shared.status.lock().unwrap();
        loop {
            if self.shared.stop.load(Ordering::Acquire) {
                return RS_IDLE;
            }
            if self.shared.face_ready.load(Ordering::Acquire) {
                return RS_SUCCESS;
            }
            if self.shared.attempt_seq.load(Ordering::Acquire) != seq0 && is_terminal(*guard) {
                return *guard;
            }
            let now = std::time::Instant::now();
            if now >= deadline {
                return RS_TIMEOUT;
            }
            let (g, _) = self
                .shared
                .cv
                .wait_timeout(guard, (deadline - now).min(Duration::from_millis(500)))
                .unwrap();
            guard = g;
        }
    }

    /// Last accepted terminal status.
    pub fn last_terminal_status(&self) -> i32 {
        *self.shared.status.lock().unwrap()
    }

    /// Cancel recognition and stop all threads.
    pub fn stop(&self) {
        self.shared.stop.store(true, Ordering::Release);
        self.shared.cv.notify_all();
        let _ = send_request(
            REQ_CANCEL_RECOGNITION,
            self.shared.latest_session.fetch_add(1, Ordering::Relaxed) + 1,
        );
        if let Some(worker) = self.worker.lock().unwrap().take() {
            let _ = worker.join();
        }
        if let Some(rx) = self.rx_thread.lock().unwrap().take() {
            let _ = rx.join();
        }
        self.shared.face_ready.store(false, Ordering::Release);
        self.shared.auto_grant.store(false, Ordering::Release);
        self.shared.manual_pending.store(false, Ordering::Release);
        cp_log("recognition: stopped");
    }
}

fn now_millis() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/// Send one request packet to the app (ephemeral socket; UDP is
/// connectionless, so any bound socket works).
fn send_request(request_type: i32, session_id: u32) -> bool {
    let tx = match UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)) {
        Ok(s) => s,
        Err(_) => return false,
    };
    let mut packet = UdpAuthRequestPacket {
        magic_number: AUTH_REQUEST_MAGIC,
        version: AUTH_REQUEST_VERSION,
        request_type,
        username_hint: [0; 64],
        timestamp: now_millis(),
        session_id,
    };
    let bytes = unsafe {
        core::slice::from_raw_parts(
            (&packet as *const UdpAuthRequestPacket).cast::<u8>(),
            core::mem::size_of::<UdpAuthRequestPacket>(),
        )
    };
    tx.send_to(bytes, (Ipv4Addr::LOCALHOST, AUTH_REQUEST_PORT)).is_ok()
}

/// Receiver thread: parse status packets; accept only packets whose
/// session_id matches the latest issued request and whose timestamp is
/// fresh (anti-injection / anti-replay).
fn recv_loop(socket: UdpSocket, shared: Arc<Shared>) {
    socket.set_read_timeout(Some(Duration::from_millis(500))).ok();
    let mut buf = vec![0u8; core::mem::size_of::<UdpStatusPacket>()];
    while !shared.stop.load(Ordering::Acquire) {
        match socket.recv_from(&mut buf) {
            Ok((n, _)) => {
                if n < 24 {
                    continue;
                }
                let magic = u32::from_le_bytes(buf[0..4].try_into().unwrap());
                let version = u32::from_le_bytes(buf[4..8].try_into().unwrap());
                if magic != STATUS_MAGIC || version != STATUS_VERSION {
                    cp_log("recognition: status packet rejected (bad magic/version)");
                    continue;
                }
                let code = i32::from_le_bytes(buf[8..12].try_into().unwrap());
                let session = u32::from_le_bytes(buf[12..16].try_into().unwrap());
                // Timestamp lives at the end of the packed struct.
                let ts_offset = core::mem::size_of::<UdpStatusPacket>() - 8;
                let ts = u64::from_le_bytes(buf[ts_offset..ts_offset + 8].try_into().unwrap());
                let now = now_millis();
                if ts > now || now.saturating_sub(ts) > STATUS_FRESHNESS_MS {
                    cp_log("recognition: status packet rejected (stale timestamp)");
                    continue;
                }
                if session != shared.latest_session.load(Ordering::Acquire) {
                    cp_log(&format!(
                        "recognition: status packet rejected (session {} != {})",
                        session,
                        shared.latest_session.load(Ordering::Acquire)
                    ));
                    continue;
                }
                *shared.status.lock().unwrap() = code;
                shared.cv.notify_all();
                cp_log(&format!("recognition: status code={} session={}", code, session));
            }
            Err(_) => continue, // read timeout or transient
        }
    }
}

fn fire_success(shared: &Shared) {
    if let Some(cb) = shared.on_success.lock().unwrap().as_ref() {
        cb();
    }
}

/// One worker loop serves both modes (RecognitionMode decides):
///
/// Manual (0): waits for the password-box Enter trigger (manual_pending),
/// runs one attempt, loops. On success GetSerialization consumes the
/// outcome synchronously; no GIT event is fired (no re-enumeration).
///
/// Auto (1): waits AutoDelaySec after the lock screen (suspension pauses
/// the countdown, resume triggers immediately, a manual Enter
/// short-circuits it), then attempts. On failure it retries every
/// RetryDelaySec; on the first success it sets face_ready and fires the
/// GIT CredentialsChanged callback exactly once (LogonUI re-enumerates and
/// auto-submits). After a success it serves manual triggers only.
fn worker_loop(cfg: RecognitionConfig, shared: Arc<Shared>) {
    let auto = cfg.mode == 1;
    let delay = Duration::from_secs(cfg.auto_delay_sec as u64);
    let retry = Duration::from_secs(cfg.retry_delay_sec as u64);
    let timeout = Duration::from_secs(cfg.timeout_sec as u64);
    let mut auto_success = false;
    let mut delay_done = !auto;

    loop {
        if shared.stop.load(Ordering::Acquire) {
            return;
        }
        let mut kind_manual = false;

        // --- Initial delay (auto mode only); a manual trigger or a
        //     resume-from-suspension short-circuits it. ---
        if !delay_done {
            let started = unsafe { windows::Win32::System::SystemInformation::GetTickCount64() };
            cp_log(&format!(
                "recognition: auto delay {}s starting (tick={})",
                cfg.auto_delay_sec, started
            ));
            loop {
                if shared.stop.load(Ordering::Acquire) {
                    return;
                }
                if shared.manual_pending.swap(false, Ordering::AcqRel) {
                    kind_manual = true;
                    break;
                }
                let now =
                    unsafe { windows::Win32::System::SystemInformation::GetTickCount64() };
                if now.wrapping_sub(started) >= delay.as_millis() as u64 {
                    break;
                }
                std::thread::sleep(Duration::from_millis(500));
                let now2 =
                    unsafe { windows::Win32::System::SystemInformation::GetTickCount64() };
                if now2.wrapping_sub(now) > 3000 {
                    cp_log("recognition: resumed from suspension, triggering now");
                    break;
                }
            }
            delay_done = true;
        } else if auto && !auto_success {
            // Auto retry path: run the next attempt now. A manual Enter
            // during the retry sleep preempts the auto attempt.
            if shared.manual_pending.swap(false, Ordering::AcqRel) {
                kind_manual = true;
            }
        } else {
            // Manual mode, or auto mode after success: wait for a manual
            // trigger (password-box Enter).
            wait_for_manual_pending(&shared);
            if shared.stop.load(Ordering::Acquire) {
                return;
            }
            kind_manual = true;
        }

        // --- One attempt. ---
        let session_id = shared.latest_session.fetch_add(1, Ordering::Relaxed) + 1;
        shared.latest_session.store(session_id, Ordering::Release);
        {
            let mut s = shared.status.lock().unwrap();
            *s = RS_RECOGNIZING;
        }
        // Status is already non-terminal (RECOGNIZING) before the sequence
        // bump, so wait_outcome cannot mistake a stale terminal status for
        // this attempt's outcome.
        shared.attempt_seq.fetch_add(1, Ordering::Release);
        let sent = send_request(REQ_START_RECOGNITION, session_id);
        let result = if sent {
            wait_terminal(&shared, timeout)
        } else {
            cp_log("recognition: START_RECOGNITION send failed");
            RS_RECOGNITION_ERROR
        };
        if result == RS_IDLE {
            return; // stopped
        }

        match result {
            RS_SUCCESS => {
                auto_success = true; // stop auto retries after any success
                if kind_manual {
                    // GetSerialization consumes the outcome synchronously;
                    // no GIT event (would trigger a needless re-enumeration).
                    cp_log("recognition: manual SUCCESS (sync submit path)");
                } else {
                    shared.face_ready.store(true, Ordering::Release);
                    cp_log("recognition: auto SUCCESS, firing CredentialsChanged");
                    fire_success(&shared);
                }
            }
            RS_TIMEOUT | RS_FAILED | RS_RECOGNITION_ERROR | RS_PROCESS_ENDED => {
                cp_log(&format!("recognition: attempt failed status={}", result));
                if auto && !auto_success {
                    sleep_interruptible(&shared, retry);
                }
            }
            _ => {}
        }
    }
}

/// Block until a manual trigger is pending or the worker is stopped.
fn wait_for_manual_pending(shared: &Arc<Shared>) {
    let mut guard = shared.status.lock().unwrap();
    while !shared.manual_pending.load(Ordering::Acquire)
        && !shared.stop.load(Ordering::Acquire)
    {
        guard = shared.cv.wait(guard).unwrap();
    }
}

/// Sleep for `duration`, but wake early on stop or a manual trigger so a
/// password-box Enter is not delayed by the auto retry cadence.
fn sleep_interruptible(shared: &Arc<Shared>, duration: Duration) {
    let deadline = std::time::Instant::now() + duration;
    let mut guard = shared.status.lock().unwrap();
    loop {
        let now = std::time::Instant::now();
        if now >= deadline || shared.stop.load(Ordering::Acquire) {
            return;
        }
        if shared.manual_pending.load(Ordering::Acquire) {
            return;
        }
        let (g, _) = shared
            .cv
            .wait_timeout(guard, (deadline - now).min(Duration::from_millis(500)))
            .unwrap();
        guard = g;
    }
}

/// Wait for a terminal status or timeout (returns RS_TIMEOUT), or RS_IDLE
/// when stopped.
fn wait_terminal(shared: &Arc<Shared>, timeout: Duration) -> i32 {
    let deadline = std::time::Instant::now() + timeout;
    let mut guard = shared.status.lock().unwrap();
    loop {
        if shared.stop.load(Ordering::Acquire) {
            return RS_IDLE;
        }
        if is_terminal(*guard) {
            return *guard;
        }
        let now = std::time::Instant::now();
        if now >= deadline {
            return RS_TIMEOUT;
        }
        let (g, _) = shared
            .cv
            .wait_timeout(guard, (deadline - now).min(Duration::from_millis(500)))
            .unwrap();
        guard = g;
    }
}

/// Terminal statuses that end one attempt.
fn is_terminal(status: i32) -> bool {
    matches!(
        status,
        RS_SUCCESS | RS_FAILED | RS_TIMEOUT | RS_RECOGNITION_ERROR | RS_PROCESS_ENDED
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn packet_sizes_match_cpp() {
        // C++: UdpAuthRequestPacket = 4+4+4+64+8+4 = 88 bytes.
        assert_eq!(core::mem::size_of::<UdpAuthRequestPacket>(), 88);
        // C++: UdpStatusPacket = 4+4+4+4+4+64+49152+8 = 49244 bytes.
        assert_eq!(
            core::mem::size_of::<UdpStatusPacket>(),
            4 + 4 + 4 + 4 + 4 + 64 + UDP_STATUS_MAX_FEATURE_BYTES + 8
        );
    }

    #[test]
    fn config_defaults_when_key_missing() {
        // load_config reads HKLM; in tests the key may or may not exist, but
        // defaults must be valid either way.
        let cfg = load_config();
        assert!(cfg.mode <= 1);
        assert!(cfg.auto_delay_sec >= 1);
        assert!(cfg.retry_delay_sec >= 1);
        assert!(cfg.timeout_sec >= 5);
    }

    /// Serializes tests that bind the fixed loopback ports (51234/51236).
    fn port_lock() -> &'static std::sync::Mutex<()> {
        static LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
        &LOCK
    }

    #[test]
    fn manual_trigger_roundtrip_with_mock_server() {
        let _guard = port_lock().lock().unwrap();
        // Full UDP round-trip: Recognition sends START_RECOGNITION to a mock
        // server on 51236; the mock answers on 51234 with the SAME session_id
        // and a fresh timestamp; trigger_and_wait must return RS_SUCCESS.
        // (Binds the real loopback ports; safe under wine and on Windows.)
        let mock = std::thread::spawn(|| {
            let sock = std::net::UdpSocket::bind(("127.0.0.1", 51236)).unwrap();
            sock.set_read_timeout(Some(std::time::Duration::from_millis(200)))
                .ok();
            let mut buf = vec![0u8; 256];
            for _ in 0..20 {
                match sock.recv_from(&mut buf) {
                    Ok((n, _)) => {
                        if n < 88 {
                            continue;
                        }
                        let magic = u32::from_le_bytes(buf[0..4].try_into().unwrap());
                        if magic != 0x4155_5448 {
                            continue;
                        }
                        let session_id =
                            u32::from_le_bytes(buf[84..88].try_into().unwrap());
                        let mut pkt = [0u8; 49244];
                        pkt[0..4].copy_from_slice(&0x8581_DAF3u32.to_le_bytes());
                        pkt[4..8].copy_from_slice(&2u32.to_le_bytes());
                        pkt[8..12].copy_from_slice(&2i32.to_le_bytes()); // RS_SUCCESS
                        pkt[12..16].copy_from_slice(&session_id.to_le_bytes());
                        let ts = std::time::SystemTime::now()
                            .duration_since(std::time::UNIX_EPOCH)
                            .unwrap()
                            .as_millis() as u64;
                        pkt[49236..49244].copy_from_slice(&ts.to_le_bytes());
                        let _ = sock.send_to(&pkt, ("127.0.0.1", 51234));
                        return;
                    }
                    Err(_) => continue,
                }
            }
        });

        // Give the mock a moment to bind, then run the client.
        std::thread::sleep(std::time::Duration::from_millis(300));
        let cfg = RecognitionConfig {
            mode: 0,
            auto_delay_sec: 1,
            retry_delay_sec: 1,
            timeout_sec: 5,
        };
        let rec = Recognition::new(cfg);
        assert!(rec.available(), "status socket must bind in test");
        rec.start_manual();
        let outcome = rec.trigger_and_wait();
        rec.stop();
        let _ = mock.join();
        assert_eq!(outcome, RS_SUCCESS, "round-trip must yield SUCCESS");
    }

    #[test]
    fn manual_trigger_returns_failure_on_mock_fail() {
        let _guard = port_lock().lock().unwrap();
        let mock = std::thread::spawn(|| {
            let sock = std::net::UdpSocket::bind(("127.0.0.1", 51236)).unwrap();
            sock.set_read_timeout(Some(std::time::Duration::from_millis(200)))
                .ok();
            let mut buf = vec![0u8; 256];
            for _ in 0..20 {
                match sock.recv_from(&mut buf) {
                    Ok((n, _)) => {
                        if n < 88 {
                            continue;
                        }
                        let magic = u32::from_le_bytes(buf[0..4].try_into().unwrap());
                        if magic != 0x4155_5448 {
                            continue;
                        }
                        let session_id =
                            u32::from_le_bytes(buf[84..88].try_into().unwrap());
                        let mut pkt = [0u8; 49244];
                        pkt[0..4].copy_from_slice(&0x8581_DAF3u32.to_le_bytes());
                        pkt[4..8].copy_from_slice(&2u32.to_le_bytes());
                        pkt[8..12].copy_from_slice(&3i32.to_le_bytes()); // RS_FAILED
                        pkt[12..16].copy_from_slice(&session_id.to_le_bytes());
                        let ts = std::time::SystemTime::now()
                            .duration_since(std::time::UNIX_EPOCH)
                            .unwrap()
                            .as_millis() as u64;
                        pkt[49236..49244].copy_from_slice(&ts.to_le_bytes());
                        let _ = sock.send_to(&pkt, ("127.0.0.1", 51234));
                        return;
                    }
                    Err(_) => continue,
                }
            }
        });

        std::thread::sleep(std::time::Duration::from_millis(300));
        let cfg = RecognitionConfig {
            mode: 0,
            auto_delay_sec: 1,
            retry_delay_sec: 1,
            timeout_sec: 5,
        };
        let rec = Recognition::new(cfg);
        assert!(rec.available());
        rec.start_manual();
        let outcome = rec.trigger_and_wait();
        rec.stop();
        let _ = mock.join();
        assert_eq!(outcome, RS_FAILED, "mock FAILED must reject the gate");
    }

    #[test]
    fn terminal_status_set() {
        assert!(is_terminal(RS_SUCCESS));
        assert!(is_terminal(RS_FAILED));
        assert!(is_terminal(RS_TIMEOUT));
        assert!(is_terminal(RS_RECOGNITION_ERROR));
        assert!(is_terminal(RS_PROCESS_ENDED));
        assert!(!is_terminal(RS_IDLE));
        assert!(!is_terminal(RS_RECOGNIZING));
        assert!(!is_terminal(RS_FACE_DETECTED));
    }
}
