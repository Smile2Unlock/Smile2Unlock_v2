//! Windows worker and COM plumbing that drives [`AttemptMachine`].
//!
//! The provider runs recognition off the LogonUI thread:
//!
//! - `Provider::Advise` registers the events interface in the process-wide
//!   Global Interface Table (GIT) and hands the resulting cookie to the
//!   runtime. The worker apartment obtains a marshaled proxy and calls
//!   `CredentialsChanged` from there, which is the only supported way to wake
//!   LogonUI without storing a raw COM pointer across apartments.
//! - one worker thread per provider process owns the service call. The pipe
//!   transaction is overlapped I/O bounded by the attempt deadline; aborting
//!   sets a manual-reset event and cancels the in-flight request with
//!   `CancelIoEx`, so deselecting or locking again never waits for a camera
//!   and never depends on `CancelSynchronousIo` succeeding.
//! - the prepared password stays in `PreparedPipePassword` protected memory
//!   and is published to the COM side only after a matching completion.
//!
//! The scheduling rules live in `auto_recognition`, which is platform
//! independent and unit tested with a fake clock.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread;
use std::time::Duration;

use windows::Win32::Foundation::{CloseHandle, ERROR_SUCCESS, HANDLE};
use windows::Win32::System::Registry::{
    HKEY, HKEY_LOCAL_MACHINE, HKEY_USERS, KEY_QUERY_VALUE, REG_DWORD, RegCloseKey, RegOpenKeyExW,
    RegQueryValueExW,
};
use windows::Win32::System::SystemInformation::GetTickCount64;
use windows::Win32::System::Threading::{CreateEventW, ResetEvent, SetEvent};
use windows_core::PCWSTR;

use crate::auto_recognition::{AttemptMachine, Completion, Outcome, Phase, Poll, TriggerSettings};

use crate::event_sink::ReadyNotifier;

/// Source of monotonic milliseconds. Injected so the worker can be exercised
/// with a deterministic clock.
pub trait Clock: Send + Sync + 'static {
    fn now_ms(&self) -> u64;
}

pub struct MonotonicClock;

impl Clock for MonotonicClock {
    fn now_ms(&self) -> u64 {
        // SAFETY: GetTickCount64 has no parameters and cannot fail.
        unsafe { GetTickCount64() }
    }
}

/// One recognition attempt performed by the transport.
pub enum TransportResult<G> {
    /// Face matched; the service released the one-time secret.
    Grant(G),
    /// Retryable outcome.
    Transient,
    /// Non-retryable outcome.
    Fatal,
}

/// Blocking recognition transport. Implemented by the pipe client; tests use
/// a fake that never touches the service. `abort` is a manual-reset event the
/// runtime signals to cancel the in-flight transaction; `deadline_ms` is the
/// absolute monotonic deadline of the whole attempt.
pub trait RecognitionTransport: Send + 'static {
    type Grant: Send + 'static;
    fn recognize(
        &mut self,
        sid: &str,
        request_id: u64,
        session_id: u32,
        abort: HANDLE,
        deadline_ms: u64,
    ) -> TransportResult<Self::Grant>;
}

/// Manual-reset event shared across threads. The raw HANDLE is only used
/// with `SetEvent`/`ResetEvent`/wait functions, which are thread-safe, so the
/// `Send`/`Sync` impl is sound.
struct SharedEvent(HANDLE);

unsafe impl Send for SharedEvent {}
unsafe impl Sync for SharedEvent {}

impl SharedEvent {
    fn new() -> Self {
        // SAFETY: no security attributes, manual-reset, initially unset.
        // CreateEventW only fails under resource exhaustion; panicking here
        // matches the surrounding Mutex::new unwrapping style.
        let handle = unsafe { CreateEventW(None, true, false, None) }
            .expect("CreateEventW for the cancel event");
        Self(handle)
    }

    fn set(&self) {
        // SAFETY: valid event handle; SetEvent is thread-safe.
        unsafe {
            let _ = SetEvent(self.0);
        }
    }

    fn reset(&self) {
        // SAFETY: valid event handle; ResetEvent is thread-safe.
        unsafe {
            let _ = ResetEvent(self.0);
        }
    }

    fn raw(&self) -> HANDLE {
        self.0
    }
}

impl Drop for SharedEvent {
    fn drop(&mut self) {
        if !self.0.is_invalid() {
            // SAFETY: created by CreateEventW, closed exactly once.
            unsafe {
                let _ = CloseHandle(self.0);
            }
        }
    }
}

struct Inner<G> {
    machine: AttemptMachine,
    grant: Option<G>,
    grant_request: Option<(u64, u32)>,
    ready_sid: Option<String>,
    notified: bool,
    shutdown: bool,
}

/// Shared automatic-recognition runtime. One instance per provider process;
/// every tile clones the same `Arc`.
pub struct AutoRuntime<T: RecognitionTransport> {
    inner: Mutex<Inner<T::Grant>>,
    wake: Condvar,
    transport: Mutex<T>,
    notifier: Mutex<Option<Arc<dyn ReadyNotifier>>>,
    worker_running: AtomicBool,
    cancel_event: SharedEvent,
    clock: Box<dyn Clock>,
}

impl<T: RecognitionTransport> AutoRuntime<T> {
    pub fn new(transport: T, clock: Box<dyn Clock>) -> Arc<Self> {
        Arc::new(Self {
            inner: Mutex::new(Inner {
                machine: AttemptMachine::new(),
                grant: None,
                grant_request: None,
                ready_sid: None,
                notified: false,
                shutdown: false,
            }),
            wake: Condvar::new(),
            transport: Mutex::new(transport),
            notifier: Mutex::new(None),
            worker_running: AtomicBool::new(false),
            cancel_event: SharedEvent::new(),
            clock,
        })
    }

    pub fn set_notifier(&self, notifier: Arc<dyn ReadyNotifier>) {
        *self.notifier.lock().unwrap_or_else(|e| e.into_inner()) = Some(notifier);
    }

    /// Re-arm a runtime after `UnAdvise`. A worker that is still exiting
    /// finishes on its own; the next selection starts a fresh one.
    pub fn arm(&self) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        inner.shutdown = false;
    }

    pub fn clear_notifier(&self) {
        *self.notifier.lock().unwrap_or_else(|e| e.into_inner()) = None;
    }

    /// Install the trigger policy for the selected user. Switching to manual
    /// while an attempt is active invalidates that attempt.
    pub fn set_settings(&self, settings: TriggerSettings) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        let was_active = inner.machine.is_active();
        inner.machine.set_settings(settings);
        if was_active && !settings.is_automatic() {
            inner.machine.cancel();
            inner.grant = None;
            inner.grant_request = None;
            inner.ready_sid = None;
            inner.notified = false;
            drop(inner);
            // Also abort an in-flight transaction so the transport is free
            // for the manual path immediately.
            self.cancel_event.set();
        }
        self.wake.notify_all();
    }

    /// Begin (or keep) an automatic attempt for `sid`. Returns the attempt
    /// generation for logging; consumption is keyed by SID because LogonUI
    /// re-enumerates after `CredentialsChanged`.
    pub fn select(self: &Arc<Self>, sid: &str, session_id: u32, request_id: u64) -> u64 {
        let (generation, started) = {
            let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
            inner
                .machine
                .begin(self.clock.now_ms(), sid, session_id, request_id)
        };
        if started {
            self.ensure_worker();
        }
        self.wake.notify_all();
        generation
    }

    /// Invalidate the current attempt and unblock the worker.
    pub fn cancel(&self) {
        {
            let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
            inner.machine.cancel();
            inner.grant = None;
            inner.grant_request = None;
            inner.ready_sid = None;
            inner.notified = false;
        }
        self.wake.notify_all();
        self.cancel_blocking_io();
    }
    /// Stop the worker permanently (provider teardown). The worker exits on
    /// its own; the caller never joins it.
    pub fn shutdown(self: &Arc<Self>) {
        {
            let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
            inner.shutdown = true;
            inner.machine.cancel();
            inner.grant = None;
            inner.grant_request = None;
            inner.ready_sid = None;
            inner.notified = false;
        }
        self.wake.notify_all();
        self.cancel_blocking_io();
    }

    pub fn phase(&self) -> Phase {
        self.inner
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .machine
            .phase()
    }

    pub fn generation(&self) -> u64 {
        self.inner
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .machine
            .generation()
    }

    pub fn is_automatic(&self) -> bool {
        self.inner
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .machine
            .settings()
            .is_automatic()
    }

    /// SID whose grant is currently published, if any. `GetCredentialCount`
    /// uses this to pick the default tile and enable autologon.
    pub fn ready_sid(&self) -> Option<String> {
        let inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if inner.machine.phase() == Phase::Ready {
            inner.ready_sid.clone()
        } else {
            None
        }
    }

    /// Consume the published grant exactly once for `generation`/`sid`.
    /// Returns the grant together with the service request identity, so a
    /// failed Windows logon can mark that exact one-time secret stale.
    pub fn take_ready(&self, sid: &str) -> Option<(T::Grant, u64, u32)> {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if inner.ready_sid.as_deref() != Some(sid) {
            return None;
        }
        let generation = inner.machine.generation();
        if !inner.machine.take_ready(generation) {
            return None;
        }
        inner.ready_sid = None;
        let (request_id, session_id) = inner.grant_request.take()?;
        inner
            .grant
            .take()
            .map(|grant| (grant, request_id, session_id))
    }

    fn notify_ready(&self) {
        // Clone the Arc and release the lock before calling into COM:
        // CredentialsChanged can re-enter the provider, and holding the
        // notifier lock across that call could deadlock teardown.
        let notifier = self
            .notifier
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .as_ref()
            .map(Arc::clone);
        if let Some(notifier) = notifier {
            notifier.notify_ready();
        }
    }

    fn cancel_blocking_io(&self) {
        // The overlapped pipe transaction waits on this event; setting it
        // makes the transport CancelIoEx its in-flight request and return
        // ERROR_OPERATION_ABORTED without needing a handle to the worker
        // thread.
        self.cancel_event.set();
    }

    fn ensure_worker(self: &Arc<Self>) {
        if self.worker_running.swap(true, Ordering::AcqRel) {
            return;
        }
        let runtime = Arc::clone(self);
        let spawned = thread::Builder::new()
            .name("su-auto-recognition".to_owned())
            .spawn(move || runtime.worker_loop());
        if spawned.is_err() {
            self.worker_running.store(false, Ordering::Release);
            crate::log::cp_log("auto worker spawn FAILED");
        }
    }

    fn worker_loop(self: &Arc<Self>) {
        // The GIT proxy is created and called in an MTA; LogonUI initialized
        // its own apartment, so this thread must join a different one.
        let _apartment = ComApartment::enter_mta();
        loop {
            let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
            if inner.shutdown {
                break;
            }
            let now = self.clock.now_ms();
            match inner.machine.poll(now) {
                Poll::Idle => {
                    drop(self.wake.wait(inner));
                }
                Poll::WaitUntil(target) => {
                    let wait = target.saturating_sub(now);
                    if wait == 0 {
                        continue;
                    }
                    drop(self.wake.wait_timeout(inner, Duration::from_millis(wait)));
                }
                Poll::Ready => {
                    if inner.notified {
                        drop(self.wake.wait(inner));
                    } else {
                        inner.notified = true;
                        drop(inner);
                        self.notify_ready();
                    }
                }
                Poll::Finished => {
                    inner.grant = None;
                    inner.grant_request = None;
                    inner.ready_sid = None;
                    drop(self.wake.wait(inner));
                }
                Poll::Recognize(request) => {
                    let sid = inner.machine.active_sid().unwrap_or_default().to_owned();
                    drop(inner);
                    // Clear any stale cancel signal from a previous attempt
                    // before handing the event to the transport; from here
                    // the transaction is bounded by (abort, deadline).
                    self.cancel_event.reset();
                    let abort = self.cancel_event.raw();
                    let outcome = {
                        let mut transport =
                            self.transport.lock().unwrap_or_else(|e| e.into_inner());
                        transport.recognize(
                            &sid,
                            request.request_id,
                            request.session_id,
                            abort,
                            request.deadline_ms,
                        )
                    };
                    let completed_at = self.clock.now_ms();
                    let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
                    match outcome {
                        TransportResult::Grant(grant) => {
                            if inner.machine.complete(
                                request.generation,
                                completed_at,
                                Outcome::Success,
                            ) == Completion::Accepted
                            {
                                inner.grant = Some(grant);
                                inner.grant_request =
                                    Some((request.request_id, request.session_id));
                                inner.ready_sid = Some(sid);
                                inner.notified = false;
                            }
                            // A stale grant is dropped here and zeroized by
                            // `PreparedPipePassword::drop`.
                        }
                        TransportResult::Transient => {
                            let _ = inner.machine.complete(
                                request.generation,
                                completed_at,
                                Outcome::Transient,
                            );
                        }
                        TransportResult::Fatal => {
                            let _ = inner.machine.complete(
                                request.generation,
                                completed_at,
                                Outcome::Fatal,
                            );
                        }
                    }
                }
            }
        }
        self.worker_running.store(false, Ordering::Release);
    }
}

/// RAII COM apartment for the worker thread. `None` means COM was already
/// initialized in a different mode, in which case we must not uninitialize.
struct ComApartment;

impl ComApartment {
    fn enter_mta() -> Option<Self> {
        // SAFETY: CoInitializeEx is balanced by CoUninitialize in Drop. If it
        // returns any failure (including RPC_E_CHANGED_MODE) COM is already
        // initialized elsewhere and must not be uninitialized here.
        let result = unsafe {
            windows::Win32::System::Com::CoInitializeEx(
                None,
                windows::Win32::System::Com::COINIT_MULTITHREADED,
            )
        };
        if result.is_ok() { Some(Self) } else { None }
    }
}

impl Drop for ComApartment {
    fn drop(&mut self) {
        // SAFETY: only constructed when CoInitializeEx returned S_OK.
        unsafe { windows::Win32::System::Com::CoUninitialize() };
    }
}

/// Transport backed by the LocalSystem logon-secret pipe.
pub struct PipeRecognitionTransport;

impl RecognitionTransport for PipeRecognitionTransport {
    type Grant = crate::pipe_client::PreparedPipePassword;

    fn recognize(
        &mut self,
        sid: &str,
        request_id: u64,
        session_id: u32,
        abort: HANDLE,
        deadline_ms: u64,
    ) -> TransportResult<Self::Grant> {
        let wide: Vec<u16> = sid.encode_utf16().collect();
        match crate::pipe_client::PipeClient.prepare_with_limits(
            &wide,
            request_id,
            session_id,
            Some(abort),
            deadline_ms,
        ) {
            Ok(password) => TransportResult::Grant(password),
            Err(error) => {
                let outcome = classify(error.code().0 as u32);
                crate::log::cp_log(&format!(
                    "auto recognize outcome={:?} hresult={:08x}",
                    outcome,
                    error.code().0
                ));
                match outcome {
                    Outcome::Transient => TransportResult::Transient,
                    _ => TransportResult::Fatal,
                }
            }
        }
    }
}

/// Map a failed recognition transaction to a retry decision.
///
/// Transient: no face, liveness not met, no profile match, camera busy and
/// pipe contention, plus cancellation/timeout races where the attempt may
/// still be alive. Everything else stops the attempt and leaves the manual
/// password tile in place.
pub fn classify(hresult: u32) -> Outcome {
    const FACILITY_WIN32: u32 = 0x8007_0000;
    let code = hresult & 0xffff;
    if (hresult & FACILITY_WIN32) == FACILITY_WIN32 {
        match code {
            // ERROR_LOGON_FAILURE: face did not match, or liveness failed.
            1326
            // ERROR_PIPE_BUSY / ERROR_SEM_TIMEOUT / ERROR_TIMEOUT.
            | 231 | 121 | 1460
            // ERROR_OPERATION_ABORTED: the abort event fired; if the attempt
            // is still active this was a spurious race, so retry rather than
            // kill.
            | 995 => return Outcome::Transient,
            _ => {}
        }
    }
    Outcome::Fatal
}

/// Machine-wide trigger settings key: `HKLM\SOFTWARE\Smile2Unlock\Recognition\<sid>`.
pub fn machine_settings_path(sid: &str) -> String {
    format!("SOFTWARE\\Smile2Unlock\\Recognition\\{sid}")
}

/// Per-user trigger settings key, written by the GUI for the logged-on user.
pub fn user_settings_path(sid: &str) -> String {
    format!("{sid}\\Software\\Smile2Unlock\\Recognition")
}

fn read_dword(key: HKEY, name: &str) -> Option<u32> {
    let name: Vec<u16> = name.encode_utf16().chain(core::iter::once(0)).collect();
    let mut value = 0u32;
    let mut size = core::mem::size_of::<u32>() as u32;
    let mut kind = REG_DWORD;
    // SAFETY: name is NUL-terminated; value/size/kind are valid out-params.
    let result = unsafe {
        RegQueryValueExW(
            key,
            PCWSTR(name.as_ptr()),
            None,
            Some(&mut kind),
            Some((&mut value as *mut u32).cast::<u8>()),
            Some(&mut size),
        )
    };
    if result == ERROR_SUCCESS && kind == REG_DWORD && size == core::mem::size_of::<u32>() as u32 {
        Some(value)
    } else {
        None
    }
}

fn read_trigger_settings_at(root: HKEY, path: &str) -> Option<TriggerSettings> {
    let path: Vec<u16> = path.encode_utf16().chain(core::iter::once(0)).collect();
    let mut key = HKEY::default();
    // SAFETY: path is NUL-terminated; key is a valid out-param.
    let opened =
        unsafe { RegOpenKeyExW(root, PCWSTR(path.as_ptr()), None, KEY_QUERY_VALUE, &mut key) };
    if opened != ERROR_SUCCESS {
        return None;
    }
    let settings = TriggerSettings::from_values(
        read_dword(key, "RecognitionMode"),
        read_dword(key, "AutoDelaySec"),
        read_dword(key, "RetryDelaySec"),
        read_dword(key, "TimeoutSec"),
    );
    // SAFETY: key was opened above and is closed exactly once.
    unsafe {
        let _ = RegCloseKey(key);
    }
    Some(settings)
}

/// Read the trigger policy for `sid`.
///
/// The machine-wide key (written by the LocalSystem service) is authoritative
/// because it survives a cold boot where the user's registry hive is not
/// loaded. The per-user key written by the GUI is a fallback for installs that
/// predate the service store. Missing values fall back to the shared defaults.
pub fn read_trigger_settings(sid: &str) -> TriggerSettings {
    if let Some(settings) =
        read_trigger_settings_at(HKEY_LOCAL_MACHINE, &machine_settings_path(sid))
    {
        return settings;
    }
    if let Some(settings) = read_trigger_settings_at(HKEY_USERS, &user_settings_path(sid)) {
        return settings;
    }
    TriggerSettings::manual()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicU32;
    use std::sync::atomic::AtomicUsize;

    struct FakeClock(AtomicU32);

    impl FakeClock {
        fn new(start_ms: u32) -> Self {
            Self(AtomicU32::new(start_ms))
        }
    }

    impl Clock for FakeClock {
        fn now_ms(&self) -> u64 {
            self.0.load(Ordering::Acquire) as u64
        }
    }

    struct FakeTransport {
        outcomes: Mutex<Vec<Outcome>>,
        calls: AtomicUsize,
    }

    impl FakeTransport {
        fn new(outcomes: Vec<Outcome>) -> Self {
            Self {
                outcomes: Mutex::new(outcomes),
                calls: AtomicUsize::new(0),
            }
        }
    }

    impl RecognitionTransport for FakeTransport {
        type Grant = &'static str;

        fn recognize(
            &mut self,
            _sid: &str,
            _request_id: u64,
            _session_id: u32,
            _abort: HANDLE,
            _deadline_ms: u64,
        ) -> TransportResult<Self::Grant> {
            self.calls.fetch_add(1, Ordering::AcqRel);
            let mut outcomes = self.outcomes.lock().unwrap();
            let outcome = if outcomes.is_empty() {
                Outcome::Fatal
            } else {
                outcomes.remove(0)
            };
            match outcome {
                Outcome::Success => TransportResult::Grant("grant"),
                Outcome::Transient => TransportResult::Transient,
                Outcome::Fatal => TransportResult::Fatal,
            }
        }
    }

    struct CountingNotifier(AtomicUsize);

    impl ReadyNotifier for CountingNotifier {
        fn notify_ready(&self) {
            self.0.fetch_add(1, Ordering::AcqRel);
        }
    }

    fn wait_for<F: Fn() -> bool>(condition: F) {
        for _ in 0..2_000 {
            if condition() {
                return;
            }
            thread::sleep(Duration::from_millis(1));
        }
        panic!("condition was not reached");
    }

    #[test]
    fn classify_retries_only_expected_outcomes() {
        assert_eq!(classify(0x8007_052E), Outcome::Transient); // 1326
        assert_eq!(classify(0x8007_00E7), Outcome::Transient); // 231 pipe busy
        assert_eq!(classify(0x8007_0079), Outcome::Transient); // 121 sem timeout
        assert_eq!(classify(0x8007_05B4), Outcome::Transient); // 1460 timeout
        assert_eq!(classify(0x8007_0005), Outcome::Fatal); // access denied
        assert_eq!(classify(0x8007_0490), Outcome::Fatal); // profile not found
        assert_eq!(classify(0x8007_0426), Outcome::Fatal); // service not active
        assert_eq!(classify(0x8007_0002), Outcome::Fatal); // file not found
        assert_eq!(classify(0x8000_4001), Outcome::Fatal); // E_NOTIMPL
    }

    #[test]
    fn registry_paths_are_per_sid() {
        assert_eq!(
            machine_settings_path("S-1-5-21-1"),
            "SOFTWARE\\Smile2Unlock\\Recognition\\S-1-5-21-1"
        );
        assert_eq!(
            user_settings_path("S-1-5-21-1"),
            "S-1-5-21-1\\Software\\Smile2Unlock\\Recognition"
        );
    }

    #[test]
    fn worker_publishes_grant_and_notifies_once() {
        let clock = Box::new(FakeClock::new(0));
        let runtime = AutoRuntime::new(FakeTransport::new(vec![Outcome::Success]), clock);
        let notifier = Arc::new(CountingNotifier(AtomicUsize::new(0)));
        runtime.set_notifier(Arc::new(NotifierHandle(Arc::clone(&notifier))));
        runtime.set_settings(TriggerSettings::from_values(
            Some(1),
            Some(0),
            Some(1),
            Some(30),
        ));
        runtime.select("S-1-5-21-1", 1, 7);
        wait_for(|| runtime.ready_sid().as_deref() == Some("S-1-5-21-1"));
        assert_eq!(notifier.0.load(Ordering::Acquire), 1);
        assert_eq!(runtime.phase(), Phase::Ready);
        // Repeated notification is suppressed while the grant waits.
        runtime.set_settings(TriggerSettings::from_values(
            Some(1),
            Some(0),
            Some(1),
            Some(30),
        ));
        thread::sleep(Duration::from_millis(20));
        assert_eq!(notifier.0.load(Ordering::Acquire), 1);
        let grant = runtime.take_ready("S-1-5-21-1");
        assert_eq!(grant.map(|(grant, _, _)| grant), Some("grant"));
        assert!(runtime.take_ready("S-1-5-21-1").is_none());
        runtime.shutdown();
    }

    #[test]
    fn worker_retries_transient_outcome() {
        let runtime = AutoRuntime::new(
            FakeTransport::new(vec![Outcome::Transient, Outcome::Success]),
            Box::new(FakeClock::new(0)),
        );
        // A zero retry delay keeps this test independent of wall-clock time;
        // the delay arithmetic itself is covered by auto_recognition tests.
        runtime.set_settings(TriggerSettings {
            mode: crate::auto_recognition::TriggerMode::Automatic,
            auto_delay: Duration::ZERO,
            retry_delay: Duration::ZERO,
            timeout: Duration::from_secs(30),
        });
        runtime.select("S-1-5-21-1", 1, 7);
        wait_for(|| runtime.phase() == Phase::Ready);
        assert_eq!(
            runtime.take_ready("S-1-5-21-1").map(|(g, _, _)| g),
            Some("grant")
        );
        runtime.shutdown();
    }

    #[test]
    fn cancel_discards_grant_and_stops_retries() {
        let runtime = AutoRuntime::new(
            FakeTransport::new(vec![Outcome::Success]),
            Box::new(FakeClock::new(0)),
        );
        runtime.set_settings(TriggerSettings::from_values(
            Some(1),
            Some(0),
            Some(1),
            Some(30),
        ));
        let generation = runtime.select("S-1-5-21-1", 1, 7);
        runtime.cancel();
        assert_ne!(runtime.generation(), generation);
        thread::sleep(Duration::from_millis(30));
        assert!(runtime.ready_sid().is_none());
        assert!(runtime.take_ready("S-1-5-21-1").is_none());
        assert_eq!(runtime.phase(), Phase::Idle);
        runtime.shutdown();
    }

    /// `Arc<CountingNotifier>` cannot implement `ReadyNotifier` directly, so
    /// wrap it.
    struct NotifierHandle(Arc<CountingNotifier>);

    impl ReadyNotifier for NotifierHandle {
        fn notify_ready(&self) {
            self.0.notify_ready();
        }
    }

    #[test]
    fn missing_settings_key_returns_none() {
        let sid = match crate::pipe_client::current_user_sid() {
            Ok(sid) => sid,
            Err(_) => return,
        };
        let path = format!("{}\\Software\\Smile2Unlock\\NoSuchKey", sid);
        assert!(read_trigger_settings_at(HKEY_USERS, &path).is_none());
    }

    #[test]
    fn user_hive_trigger_settings_are_normalized() {
        use windows::Win32::System::Registry::{
            KEY_SET_VALUE, REG_OPTION_NON_VOLATILE, RegCreateKeyExW, RegDeleteKeyW, RegSetValueExW,
        };
        let sid = match crate::pipe_client::current_user_sid() {
            Ok(sid) => sid,
            Err(_) => return,
        };
        let path = user_settings_path(&sid);
        let wide: Vec<u16> = path.encode_utf16().chain(core::iter::once(0)).collect();
        let mut key = HKEY::default();
        // SAFETY: all pointers are valid out-params for the duration of the
        // call; the key is closed below.
        let created = unsafe {
            RegCreateKeyExW(
                HKEY_USERS,
                PCWSTR(wide.as_ptr()),
                None,
                PCWSTR::null(),
                REG_OPTION_NON_VOLATILE,
                KEY_SET_VALUE,
                None,
                &mut key,
                None,
            )
        };
        if created != ERROR_SUCCESS {
            // Some hosts do not expose a writable HKEY_USERS hive; value
            // parsing is still covered by the pure unit tests.
            return;
        }
        let set = |name: &str, value: u32| {
            let name: Vec<u16> = name.encode_utf16().chain(core::iter::once(0)).collect();
            // SAFETY: name is NUL-terminated and value outlives the call.
            unsafe {
                RegSetValueExW(
                    key,
                    PCWSTR(name.as_ptr()),
                    None,
                    REG_DWORD,
                    Some(&value.to_ne_bytes()),
                )
            }
        };
        assert_eq!(set("RecognitionMode", 1), ERROR_SUCCESS);
        assert_eq!(set("AutoDelaySec", 0), ERROR_SUCCESS);
        assert_eq!(set("RetryDelaySec", 2), ERROR_SUCCESS);
        assert_eq!(set("TimeoutSec", 45), ERROR_SUCCESS);
        // SAFETY: key was created above.
        unsafe {
            let _ = RegCloseKey(key);
        }

        let settings = read_trigger_settings_at(HKEY_USERS, &path).expect("key exists");
        assert!(settings.is_automatic());
        assert_eq!(settings.auto_delay_ms(), 0);
        assert_eq!(settings.retry_delay_ms(), 2_000);
        assert_eq!(settings.timeout_ms(), 45_000);

        // Clean up the leaf key so the test is repeatable.
        // SAFETY: path is NUL-terminated; deleting our own leaf key.
        unsafe {
            let _ = RegDeleteKeyW(HKEY_USERS, PCWSTR(wide.as_ptr()));
        }
    }
}
