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
    owner: Option<(u64, String)>,
    initial_hint: bool,
    password_entry: bool,
    login_failed: bool,
    login_complete: bool,
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
                owner: None,
                initial_hint: true,
                password_entry: false,
                login_failed: false,
                login_complete: false,
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

    #[cfg(test)]
    pub(crate) fn publish_test_grant(&self, owner: u64, sid: &str, grant: T::Grant) {
        // Complete the real state-machine transitions without starting a
        // camera worker. Manual capture uses the same ready/submission path.
        let mut inner = self.inner.lock().unwrap();
        let now = self.clock.now_ms();
        inner.machine.set_settings(TriggerSettings::manual());
        let (generation, _) = inner.machine.begin_on_demand(now, sid, 1, 7);
        assert!(matches!(inner.machine.poll(now), Poll::Recognize(_)));
        assert_eq!(
            inner.machine.complete(generation, now, Outcome::Success),
            Completion::Accepted
        );
        inner.owner = Some((owner, sid.to_owned()));
        inner.grant = Some(grant);
        inner.grant_request = Some((7, 1));
        inner.ready_sid = Some(sid.to_owned());
    }

    /// Re-arm a runtime after `UnAdvise`. A worker that is still exiting
    /// finishes on its own; the next selection starts a fresh one.
    pub fn arm(&self) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if inner.shutdown {
            inner.initial_hint = true;
            inner.password_entry = false;
            inner.login_failed = false;
            inner.login_complete = false;
        }
        inner.shutdown = false;
    }

    pub fn clear_notifier(&self) {
        *self.notifier.lock().unwrap_or_else(|e| e.into_inner()) = None;
    }

    fn clear_grant(inner: &mut Inner<T::Grant>) {
        inner.grant = None;
        inner.grant_request = None;
        inner.ready_sid = None;
        inner.notified = false;
    }

    fn expire_locked(&self, inner: &mut Inner<T::Grant>) {
        if inner.machine.expire(self.clock.now_ms()) {
            Self::clear_grant(inner);
            self.cancel_event.set();
            self.wake.notify_all();
        }
    }

    pub fn set_settings(&self, settings: TriggerSettings) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if inner.machine.is_active()
            && inner.machine.settings().is_automatic()
            && !settings.is_automatic()
        {
            inner.machine.cancel();
            Self::clear_grant(&mut inner);
            self.cancel_event.set();
        }
        inner.machine.set_settings(settings);
        self.wake.notify_all();
    }

    pub fn select(self: &Arc<Self>, sid: &str, session_id: u32, request_id: u64) -> u64 {
        let settings = self
            .inner
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .machine
            .settings();
        self.select_owned(0, sid, session_id, request_id, settings)
    }

    /// Policy and ownership change atomically. An old tile cannot cancel a
    /// newer tile, including a replacement with the same SID.
    pub(crate) fn select_owned(
        self: &Arc<Self>,
        owner: u64,
        sid: &str,
        session_id: u32,
        request_id: u64,
        settings: TriggerSettings,
    ) -> u64 {
        let (generation, started) = {
            let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
            if inner.shutdown {
                return inner.machine.generation();
            }
            inner.initial_hint = false;
            let same_owner =
                inner.owner.as_ref().map(|(id, s)| (*id, s.as_str())) == Some((owner, sid));
            if !same_owner {
                inner.password_entry = false;
                inner.login_failed = false;
                inner.login_complete = false;
            }
            if !same_owner
                || (inner.machine.is_active()
                    && inner.machine.settings().is_automatic()
                    && !settings.is_automatic())
            {
                inner.machine.cancel();
                Self::clear_grant(&mut inner);
                self.cancel_event.set();
            }
            inner.owner = Some((owner, sid.to_owned()));
            inner.machine.set_settings(settings);
            if same_owner && matches!(inner.machine.phase(), Phase::Stopped | Phase::Submitted) {
                // CredentialsChanged can cause another SetSelected without a
                // real user deselection. It must not re-arm a failed/expired
                // attempt or create work while Windows is logging on.
                (inner.machine.generation(), false)
            } else {
                inner
                    .machine
                    .begin(self.clock.now_ms(), sid, session_id, request_id)
            }
        };
        if started {
            self.ensure_worker();
        }
        self.wake.notify_all();
        generation
    }

    /// Empty-password submit uses the worker too. A pending automatic attempt
    /// is retained; it never spawns a competing capture or resets its budget.
    pub(crate) fn submit_owned(
        self: &Arc<Self>,
        owner: u64,
        sid: &str,
        session_id: u32,
        request_id: u64,
    ) -> bool {
        let started = {
            let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
            if inner.shutdown
                || inner.owner.as_ref().map(|(id, s)| (*id, s.as_str())) != Some((owner, sid))
            {
                return false;
            }
            self.expire_locked(&mut inner);
            if inner.machine.is_active() {
                return true;
            }
            if inner.machine.phase() == Phase::Submitted
                || (inner.machine.phase() == Phase::Stopped
                    && inner.machine.settings().is_automatic())
            {
                return false;
            }
            Self::clear_grant(&mut inner);
            inner.login_failed = false;
            inner.login_complete = false;
            inner
                .machine
                .begin_on_demand(self.clock.now_ms(), sid, session_id, request_id)
                .1
        };
        if started {
            self.ensure_worker();
        }
        self.wake.notify_all();
        started
    }

    pub(crate) fn cancel_owned(&self, owner: u64) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if inner.owner.as_ref().map(|(id, _)| *id) != Some(owner) {
            return;
        }
        inner.machine.cancel();
        Self::clear_grant(&mut inner);
        // Signal while holding the state lock, paired with the worker reset.
        self.cancel_event.set();
        self.wake.notify_all();
    }

    pub(crate) fn deselect_owned(&self, owner: u64) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if inner.owner.as_ref().map(|(id, _)| *id) != Some(owner) {
            return;
        }
        inner.machine.cancel();
        Self::clear_grant(&mut inner);
        inner.owner = None;
        self.cancel_event.set();
        self.wake.notify_all();
    }

    pub fn cancel(&self) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        inner.machine.cancel();
        Self::clear_grant(&mut inner);
        inner.owner = None;
        self.cancel_event.set();
        self.wake.notify_all();
    }

    pub fn shutdown(self: &Arc<Self>) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        inner.shutdown = true;
        inner.machine.cancel();
        Self::clear_grant(&mut inner);
        inner.owner = None;
        self.cancel_event.set();
        self.wake.notify_all();
    }

    pub(crate) fn selected_sid(&self) -> Option<String> {
        self.inner
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .owner
            .as_ref()
            .map(|(_, sid)| sid.clone())
    }

    pub(crate) fn phase_owned(&self, owner: u64) -> Option<Phase> {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        self.expire_locked(&mut inner);
        (inner.owner.as_ref().map(|(id, _)| *id) == Some(owner)).then(|| inner.machine.phase())
    }

    /// A display-only snapshot: no identity, camera data or credential leaves
    /// this runtime. Reading the display never advances authentication.
    pub(crate) fn display_snapshot(&self) -> u32 {
        let inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        crate::status::snapshot(
            &inner.machine,
            self.clock.now_ms(),
            // Keep the display alive on the initial LogonUI clock surface.
            // The selected user owner is established only after the tile is
            // opened; before that point the state machine renders Waiting.
            !inner.shutdown
                && (inner.owner.is_some() || inner.initial_hint)
                && !inner.password_entry
                && !inner.login_complete,
            inner.login_failed,
        )
    }

    pub(crate) fn password_entry_owned(&self, owner: u64, entered: bool) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if inner.owner.as_ref().map(|(id, _)| *id) == Some(owner) {
            inner.password_entry = entered;
            inner.login_failed = false;
        }
    }

    pub(crate) fn logon_result_owned(&self, owner: u64, success: bool) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if inner.owner.as_ref().map(|(id, _)| *id) == Some(owner) {
            inner.login_complete = success;
            inner.login_failed = !success;
        }
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
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        self.expire_locked(&mut inner);
        if inner.machine.phase() == Phase::Ready {
            inner.ready_sid.clone()
        } else {
            None
        }
    }

    pub(crate) fn has_ready_owned(&self, owner: u64, sid: &str) -> bool {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        self.expire_locked(&mut inner);
        inner.machine.phase() == Phase::Ready
            && inner.owner.as_ref().map(|(id, _)| *id) == Some(owner)
            && inner.ready_sid.as_deref() == Some(sid)
            && inner.grant.is_some()
    }

    /// Consume the published grant exactly once for `generation`/`sid`.
    /// Returns the grant together with the service request identity, so a
    /// failed Windows logon can mark that exact one-time secret stale.
    pub fn take_ready(&self, sid: &str) -> Option<(T::Grant, u64, u32)> {
        self.take_ready_for(sid, None)
    }

    pub(crate) fn take_ready_owned(&self, owner: u64, sid: &str) -> Option<(T::Grant, u64, u32)> {
        self.take_ready_for(sid, Some(owner))
    }

    fn take_ready_for(&self, sid: &str, owner: Option<u64>) -> Option<(T::Grant, u64, u32)> {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        self.expire_locked(&mut inner);
        if owner.is_some() && inner.owner.as_ref().map(|(id, _)| *id) != owner {
            return None;
        }
        if inner.ready_sid.as_deref() != Some(sid) {
            return None;
        }
        let generation = inner.machine.generation();
        if !inner.machine.take_ready(generation, self.clock.now_ms()) {
            Self::clear_grant(&mut inner);
            self.wake.notify_all();
            return None;
        }
        inner.ready_sid = None;
        self.wake.notify_all();
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
                // Keep the transition atomic with a concurrent arm/select.
                self.worker_running.store(false, Ordering::Release);
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
                        let remaining = inner.machine.deadline_ms().saturating_sub(now);
                        drop(
                            self.wake
                                .wait_timeout(inner, Duration::from_millis(remaining)),
                        );
                    } else {
                        inner.notified = true;
                        drop(inner);
                        self.notify_ready();
                    }
                }
                Poll::Finished => {
                    Self::clear_grant(&mut inner);
                    drop(inner);
                    // Refresh the cached tile so manual-submit failures and
                    // timeout show the password fallback without blocking UI.
                    self.notify_ready();
                }
                Poll::Recognize(request) => {
                    let sid = inner.machine.active_sid().unwrap_or_default().to_owned();
                    // Reset under the same lock used by cancellation. A
                    // cancellation after this point cannot be lost.
                    self.cancel_event.reset();
                    drop(inner);
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
                    if inner.machine.generation() == request.generation
                        && inner.machine.phase() == Phase::Stopped
                    {
                        Self::clear_grant(&mut inner);
                        drop(inner);
                        self.notify_ready();
                    }
                }
            }
        }
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
        // Never overwrite/delete the logged-in user's real recognition policy.
        let path = format!(
            "{}\\RuntimeTest-{}",
            user_settings_path(&sid),
            std::process::id()
        );
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

    struct AdjustableClock(Arc<std::sync::atomic::AtomicU64>);
    impl Clock for AdjustableClock {
        fn now_ms(&self) -> u64 {
            self.0.load(Ordering::Acquire)
        }
    }

    #[test]
    fn display_visibility_and_results_belong_to_the_selected_owner() {
        let runtime = AutoRuntime::new(
            FakeTransport::new(vec![Outcome::Success]),
            Box::new(FakeClock::new(0)),
        );
        let phase = || runtime.display_snapshot() & 15;
        assert_eq!(phase(), 1, "the initial clock surface has a waiting hint");
        runtime.select_owned(10, "S-1-5-21-1", 1, 7, TriggerSettings::manual());
        assert_eq!(phase(), 1);
        runtime.password_entry_owned(10, true);
        assert_eq!(phase(), 0);
        runtime.select_owned(20, "S-1-5-21-2", 1, 8, TriggerSettings::manual());
        runtime.password_entry_owned(10, true);
        runtime.logon_result_owned(10, true);
        assert_eq!(phase(), 1, "old tile cannot hide the new user's hint");
        runtime.logon_result_owned(20, false);
        assert_eq!(phase(), 10);
        assert!(runtime.submit_owned(20, "S-1-5-21-2", 1, 9));
        wait_for(|| runtime.phase() == Phase::Ready);
        assert_eq!(phase(), 5, "a new manual attempt clears the old error");
        assert!(runtime.take_ready_owned(20, "S-1-5-21-2").is_some());
        assert_eq!(phase(), 6);
        runtime.logon_result_owned(20, true);
        assert_eq!(phase(), 0, "Windows success hides the hint");
        runtime.deselect_owned(20);
        assert_eq!(phase(), 0, "switching sign-in methods hides the hint");
        runtime.select_owned(20, "S-1-5-21-2", 1, 10, TriggerSettings::manual());
        assert_eq!(phase(), 1, "a fresh selection clears completed state");
        runtime.shutdown();
        assert_eq!(phase(), 0);
        runtime.arm();
        assert_eq!(phase(), 1, "a new LogonUI advice restores the initial hint");
    }

    #[test]
    fn switching_owner_aborts_capture_and_next_capture_gets_a_reset_event() {
        use windows::Win32::Foundation::{WAIT_OBJECT_0, WAIT_TIMEOUT};
        use windows::Win32::System::Threading::WaitForSingleObject;

        struct AbortThenGrant {
            calls: Arc<AtomicUsize>,
            aborted: Arc<AtomicBool>,
        }
        impl RecognitionTransport for AbortThenGrant {
            type Grant = &'static str;
            fn recognize(
                &mut self,
                _sid: &str,
                _request_id: u64,
                _session_id: u32,
                abort: HANDLE,
                _deadline_ms: u64,
            ) -> TransportResult<Self::Grant> {
                if self.calls.fetch_add(1, Ordering::AcqRel) == 0 {
                    // Wait on the actual event supplied to the pipe transport.
                    let cancelled = unsafe { WaitForSingleObject(abort, 2_000) };
                    self.aborted
                        .store(cancelled == WAIT_OBJECT_0, Ordering::Release);
                    TransportResult::Transient
                } else if unsafe { WaitForSingleObject(abort, 0) } == WAIT_TIMEOUT {
                    TransportResult::Grant("new-owner-grant")
                } else {
                    TransportResult::Fatal
                }
            }
        }
        let calls = Arc::new(AtomicUsize::new(0));
        let aborted = Arc::new(AtomicBool::new(false));
        let runtime = AutoRuntime::new(
            AbortThenGrant {
                calls: calls.clone(),
                aborted: aborted.clone(),
            },
            Box::new(MonotonicClock),
        );
        let settings = TriggerSettings::from_values(Some(1), Some(0), Some(1), Some(30));
        runtime.select_owned(10, "S-1-5-21-1", 1, 7, settings);
        wait_for(|| calls.load(Ordering::Acquire) == 1);
        runtime.select_owned(20, "S-1-5-21-2", 1, 8, settings);
        runtime.deselect_owned(10);
        wait_for(|| runtime.phase() == Phase::Ready);
        assert!(aborted.load(Ordering::Acquire));
        assert_eq!(calls.load(Ordering::Acquire), 2);
        assert!(runtime.take_ready_owned(10, "S-1-5-21-1").is_none());
        assert_eq!(
            runtime.take_ready_owned(20, "S-1-5-21-2"),
            Some(("new-owner-grant", 8, 1))
        );
        runtime.shutdown();
    }

    #[test]
    fn publication_and_consumption_reject_expired_grants() {
        for use_publication in [true, false] {
            let tick = Arc::new(std::sync::atomic::AtomicU64::new(0));
            let runtime = AutoRuntime::new(
                FakeTransport::new(vec![Outcome::Success]),
                Box::new(AdjustableClock(tick.clone())),
            );
            runtime.set_settings(TriggerSettings::from_values(
                Some(1),
                Some(0),
                Some(1),
                Some(5),
            ));
            runtime.select("S-1-5-21-1", 1, 7);
            wait_for(|| runtime.phase() == Phase::Ready);
            assert!(runtime.has_ready_owned(0, "S-1-5-21-1"));
            assert!(!runtime.has_ready_owned(99, "S-1-5-21-1"));
            assert!(!runtime.has_ready_owned(0, "S-1-5-21-other"));
            tick.store(5_000, Ordering::Release);
            assert!(!runtime.has_ready_owned(0, "S-1-5-21-1"));
            if use_publication {
                assert!(runtime.ready_sid().is_none());
            }
            assert!(runtime.take_ready("S-1-5-21-1").is_none());
            let inner = runtime.inner.lock().unwrap();
            assert!(inner.grant.is_none());
            assert!(inner.grant_request.is_none());
            drop(inner);
            runtime.shutdown();
        }
    }

    #[test]
    fn worker_erases_ready_grant_at_deadline_without_ui_calls() {
        let runtime = AutoRuntime::new(
            FakeTransport::new(vec![Outcome::Success]),
            Box::new(MonotonicClock),
        );
        runtime.set_settings(TriggerSettings {
            mode: crate::TriggerMode::Automatic,
            auto_delay: Duration::ZERO,
            retry_delay: Duration::from_millis(10),
            timeout: Duration::from_millis(300),
        });
        runtime.select("S-1-5-21-1", 1, 7);
        wait_for(|| runtime.phase() == Phase::Ready);
        wait_for(|| runtime.phase() == Phase::Stopped);
        assert!(runtime.inner.lock().unwrap().grant.is_none());
        runtime.shutdown();
    }

    #[test]
    fn old_owner_cannot_cancel_or_consume_replacement_even_with_same_sid() {
        let runtime = AutoRuntime::new(
            FakeTransport::new(vec![Outcome::Success]),
            Box::new(FakeClock::new(0)),
        );
        let settings = TriggerSettings::from_values(Some(1), Some(60), Some(1), Some(600));
        runtime.select_owned(10, "S-1-5-21-1", 1, 7, settings);
        runtime.select_owned(20, "S-1-5-21-2", 1, 8, settings);
        let generation = runtime.generation();
        runtime.cancel_owned(10);
        runtime.deselect_owned(10);
        assert_eq!(runtime.generation(), generation);
        assert_eq!(runtime.phase(), Phase::InitialDelay);
        runtime.select_owned(
            30,
            "S-1-5-21-2",
            1,
            9,
            TriggerSettings::from_values(Some(1), Some(0), Some(1), Some(30)),
        );
        wait_for(|| runtime.phase() == Phase::Ready);
        runtime.deselect_owned(20);
        assert!(runtime.take_ready_owned(20, "S-1-5-21-2").is_none());
        assert!(runtime.take_ready_owned(30, "S-1-5-21-2").is_some());
        runtime.shutdown();
    }

    #[test]
    fn submit_keeps_pending_attempt_and_manual_mode_uses_same_worker() {
        let runtime = AutoRuntime::new(
            FakeTransport::new(vec![Outcome::Success]),
            Box::new(FakeClock::new(0)),
        );
        let settings = TriggerSettings::from_values(Some(1), Some(60), Some(1), Some(600));
        let generation = runtime.select_owned(10, "S-1-5-21-1", 1, 7, settings);
        assert!(runtime.submit_owned(10, "S-1-5-21-1", 1, 8));
        assert_eq!(runtime.generation(), generation);
        assert_eq!(
            runtime
                .transport
                .lock()
                .unwrap()
                .calls
                .load(Ordering::Acquire),
            0
        );
        runtime.select_owned(20, "S-1-5-21-2", 1, 9, TriggerSettings::manual());
        assert_eq!(runtime.phase(), Phase::Idle);
        assert!(!runtime.submit_owned(10, "S-1-5-21-1", 1, 10));
        assert!(runtime.submit_owned(20, "S-1-5-21-2", 1, 11));
        wait_for(|| runtime.phase() == Phase::Ready);
        assert_eq!(
            runtime
                .transport
                .lock()
                .unwrap()
                .calls
                .load(Ordering::Acquire),
            1
        );
        assert_eq!(
            runtime.take_ready_owned(20, "S-1-5-21-2"),
            Some(("grant", 11, 1))
        );
        runtime.shutdown();
    }

    #[test]
    fn hard_failure_does_not_retry_or_rearm_on_serialization() {
        let runtime = AutoRuntime::new(
            FakeTransport::new(vec![Outcome::Fatal, Outcome::Success]),
            Box::new(FakeClock::new(0)),
        );
        runtime.select_owned(
            10,
            "S-1-5-21-1",
            1,
            7,
            TriggerSettings::from_values(Some(1), Some(0), Some(1), Some(30)),
        );
        wait_for(|| runtime.phase() == Phase::Stopped);
        assert!(!runtime.submit_owned(10, "S-1-5-21-1", 1, 8));
        assert_eq!(
            runtime
                .transport
                .lock()
                .unwrap()
                .calls
                .load(Ordering::Acquire),
            1
        );
        runtime.shutdown();
    }

    #[test]
    fn service_wire_hard_failures_stop_and_capture_failures_retry() {
        use crate::pipe_client::{
            kStatusAccessDenied, kStatusAuthenticationFailed, kStatusCorrupt,
            kStatusInvalidRequest, kStatusUnavailable, status_to_hresult,
        };
        for status in [
            kStatusUnavailable,
            kStatusAccessDenied,
            kStatusCorrupt,
            kStatusInvalidRequest,
        ] {
            assert_eq!(
                classify(status_to_hresult(status).code().0 as u32),
                Outcome::Fatal
            );
        }
        assert_eq!(
            classify(status_to_hresult(kStatusAuthenticationFailed).code().0 as u32),
            Outcome::Transient
        );
    }
    #[test]
    fn reselecting_after_refresh_preserves_manual_grant_and_does_not_rearm_failure() {
        let runtime = AutoRuntime::new(
            FakeTransport::new(vec![Outcome::Success, Outcome::Fatal]),
            Box::new(FakeClock::new(0)),
        );
        runtime.select_owned(10, "S-1-5-21-1", 1, 7, TriggerSettings::manual());
        assert!(runtime.submit_owned(10, "S-1-5-21-1", 1, 8));
        wait_for(|| runtime.phase() == Phase::Ready);
        let generation = runtime.generation();
        runtime.select_owned(10, "S-1-5-21-1", 1, 9, TriggerSettings::manual());
        assert_eq!(runtime.generation(), generation);
        assert!(runtime.take_ready_owned(10, "S-1-5-21-1").is_some());
        runtime.deselect_owned(10);
        let auto = TriggerSettings::from_values(Some(1), Some(0), Some(1), Some(30));
        runtime.select_owned(10, "S-1-5-21-1", 1, 10, auto);
        wait_for(|| runtime.phase() == Phase::Stopped);
        let failed_generation = runtime.generation();
        runtime.select_owned(10, "S-1-5-21-1", 1, 11, auto);
        assert_eq!(runtime.phase(), Phase::Stopped);
        assert_eq!(runtime.generation(), failed_generation);
        assert_eq!(
            runtime
                .transport
                .lock()
                .unwrap()
                .calls
                .load(Ordering::Acquire),
            2
        );
        runtime.shutdown();
    }
}
