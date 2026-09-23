// Wire constants mirror the C++ protocol-header naming (kMagic, kSidCapacity,
// ...) so the mapping to src/platform/windows/auth_service/logon_secret_protocol.h
// stays greppable; the casing lint is disabled for this module.
#![allow(non_upper_case_globals)]

//! Named-pipe client for the LocalSystem Smile2Unlock logon-secret service.
//!
//! Wire format: fixed-size binary structs shared with the C++ service
//! (src/platform/windows/auth_service/logon_secret_protocol.h). One
//! message-mode transaction per request over an overlapped pipe handle:
//! the write and the read each wait on `(io completion, abort event,
//! deadline)`, so cancellation (`CancelIoEx`) and the attempt deadline are
//! enforced without depending on `CancelSynchronousIo` succeeding.
//!
//! Security posture:
//! - kPrepare / kMarkStale are only honored by the service when the caller
//!   is LocalSystem, so this client is only meaningful from LogonUI.
//! - Passwords are held in fixed-capacity buffers and wiped with volatile
//!   stores on every path (success, failure, drop).

use windows::Win32::Foundation::{
    CloseHandle, GENERIC_READ, GENERIC_WRITE, GetLastError, HANDLE, WAIT_OBJECT_0, WAIT_TIMEOUT,
};
use windows::Win32::Security::Authorization::ConvertSidToStringSidW;
use windows::Win32::Security::{GetTokenInformation, TOKEN_QUERY, TOKEN_USER, TokenUser};
use windows::Win32::Storage::FileSystem::{
    CreateFileW, FILE_FLAG_OVERLAPPED, FILE_SHARE_NONE, OPEN_EXISTING, ReadFile, WriteFile,
};
use windows::Win32::System::IO::{CancelIoEx, GetOverlappedResult, OVERLAPPED};
use windows::Win32::System::Pipes::{
    PIPE_READMODE_MESSAGE, SetNamedPipeHandleState, WaitNamedPipeW,
};
use windows::Win32::System::SystemInformation::GetTickCount64;
use windows::Win32::System::Threading::{
    CreateEventW, GetCurrentProcess, OpenProcessToken, WaitForMultipleObjects, WaitForSingleObject,
};
use windows_core::{Error, HRESULT, PCWSTR, PWSTR};

/// HRESULT_FROM_WIN32(code): error codes with the FACILITY_WIN32 severity bit.
/// windows-core 0.62 split `Error` into windows-result 0.4.1, which exposes
/// only `Error::from_hresult`; this restores the familiar helper.
pub fn win32_error(code: u32) -> Error {
    Error::from_hresult(HRESULT((0x8007_0000u32 | (code & 0xffff)) as i32))
}

pub const kMagic: u32 = 0x5332_5350; // "S2SP"
pub const kVersion: u16 = 2;
pub const kPipeName: PCWSTR = windows_core::w!(r"\\.\pipe\Smile2Unlock.LogonSecret.v1");

#[cfg(test)]
pub const kOperationPrepare: u16 = 1;
pub const kOperationMarkStale: u16 = 2;
#[allow(dead_code)] // Phase 5 (store/clear flow)
pub const kOperationStore: u16 = 3;
#[allow(dead_code)] // Phase 5 (store/clear flow)
pub const kOperationClear: u16 = 4;
pub const kOperationAuthenticateAndPrepare: u16 = 5;

pub const kStatusOk: u32 = 0;
pub const kStatusInvalidRequest: u32 = 1;
pub const kStatusAccessDenied: u32 = 2;
pub const kStatusUnavailable: u32 = 3;
pub const kStatusStaleOrConsumed: u32 = 4;
pub const kStatusCorrupt: u32 = 5;
pub const kStatusAuthenticationFailed: u32 = 6;
pub const kStatusProfileNotFound: u32 = 7;

pub const kSidCapacity: usize = 185;
pub const kUsernameCapacity: usize = 513;
pub const kPasswordCapacity: usize = 513;
pub const kPayloadCapacity: usize = 48 * 1024;

const kWin32ErrorInvalidData: u32 = 13;
const kWin32ErrorPasswordRestriction: u32 = 37;
const kWin32ErrorServiceNotActive: u32 = 1062;
const kWin32ErrorSemTimeout: u32 = 121;
const kWin32ErrorPipeBusy: u32 = 231;
const kWin32ErrorOperationAborted: u32 = 995;
const kWin32ErrorIoPending: u32 = 997;
const kWin32ErrorTimeout: u32 = 1460;

/// Overall budget for the manual (user-initiated) submission path. The
/// service clamps one recognition transaction to roughly 33 s worst case;
/// 60 s bounds a hung service without changing interactive behavior.
const kManualDeadlineMs: u64 = 60_000;
/// Slice used while waiting for a free pipe instance, so abort and the
/// deadline stay responsive during connection contention.
const kConnectWaitSliceMs: u32 = 250;

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Request {
    pub magic: u32,
    pub version: u16,
    pub operation: u16,
    pub request_id: u64,
    pub logon_session_id: u32,
    pub account_kind: u32,
    pub sid: [u16; kSidCapacity],
    pub canonical_username: [u16; kUsernameCapacity],
    pub password: [u16; kPasswordCapacity],
    pub payload_length: u32,
    pub payload: [u8; kPayloadCapacity],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Response {
    pub magic: u32,
    pub version: u16,
    pub reserved: u16,
    pub request_id: u64,
    pub logon_session_id: u32,
    pub status: u32,
    pub password_length: u32,
    pub password: [u16; kPasswordCapacity],
    pub payload_length: u32,
    pub payload: [u8; kPayloadCapacity],
}

impl Request {
    pub fn new(operation: u16, request_id: u64, logon_session_id: u32) -> Self {
        // The C++ protocol header fills magic/version via default member
        // initializers; Rust #[repr(C)] structs have none, so they must be
        // set explicitly or the service rejects the request.
        Self {
            magic: kMagic,
            version: kVersion,
            operation,
            request_id,
            logon_session_id,
            account_kind: 0,
            sid: [0; kSidCapacity],
            canonical_username: [0; kUsernameCapacity],
            password: [0; kPasswordCapacity],
            payload_length: 0,
            payload: [0; kPayloadCapacity],
        }
    }

    pub fn request_bytes(&self) -> &[u8] {
        // Safety: #[repr(C)] with only plain integer/array fields.
        unsafe {
            core::slice::from_raw_parts(
                (self as *const Self).cast::<u8>(),
                core::mem::size_of::<Self>(),
            )
        }
    }

    pub fn clear_password(&mut self) {
        secure_clear(&mut self.password);
        secure_clear_bytes(&mut self.payload);
        self.payload_length = 0;
    }
}

impl Response {
    pub fn response_bytes_mut(&mut self) -> &mut [u8] {
        // Safety: #[repr(C)] with only plain integer/array fields.
        unsafe {
            core::slice::from_raw_parts_mut(
                (self as *mut Self).cast::<u8>(),
                core::mem::size_of::<Self>(),
            )
        }
    }

    pub fn clear_password(&mut self) {
        secure_clear(&mut self.password);
        secure_clear_bytes(&mut self.payload);
        self.password_length = 0;
        self.payload_length = 0;
    }
}

/// Wipe a fixed-capacity buffer with volatile stores so the optimizer cannot
/// elide the clears (same technique as secret_buffer.rs).
pub fn secure_clear(buf: &mut [u16]) {
    for unit in buf.iter_mut() {
        // Safety: writes through a volatile pointer; never elided by DSE.
        unsafe { core::ptr::write_volatile(unit as *mut u16, 0) };
    }
}

pub fn secure_clear_bytes(buf: &mut [u8]) {
    for byte in buf.iter_mut() {
        // Safety: the pointer is valid and volatile prevents dead-store removal.
        unsafe { core::ptr::write_volatile(byte as *mut u8, 0) };
    }
}

/// Copy a UTF-16 string into a fixed-capacity field, validating that it is
/// non-empty and strictly shorter than the capacity (room for the NUL).
pub fn copy_fixed(src: &[u16], dst: &mut [u16]) -> bool {
    if src.is_empty() || src.len() >= dst.len() {
        return false;
    }
    dst[..src.len()].copy_from_slice(src);
    dst[src.len()] = 0;
    true
}

pub fn status_to_hresult(status: u32) -> Error {
    match status {
        kStatusOk => Error::from_hresult(HRESULT(0)),
        kStatusInvalidRequest => win32_error(0x57), // E_INVALIDARG
        kStatusAccessDenied => win32_error(5),      // E_ACCESSDENIED
        kStatusStaleOrConsumed => win32_error(kWin32ErrorPasswordRestriction),
        kStatusCorrupt => win32_error(kWin32ErrorInvalidData),
        kStatusAuthenticationFailed => win32_error(1326), // ERROR_LOGON_FAILURE
        kStatusProfileNotFound => win32_error(1168),      // ERROR_NOT_FOUND
        kStatusUnavailable => win32_error(kWin32ErrorServiceNotActive),
        _ => win32_error(kWin32ErrorInvalidData),
    }
}

/// Validate a received response against the request and the protocol.
/// `bytes_read` is the CallNamedPipeW reported byte count; it must equal the
/// full response size (message-mode pipe guarantees atomic messages).
pub fn validate_response(
    response: &Response,
    request: &Request,
    bytes_read: u32,
) -> Result<(), Error> {
    if bytes_read as usize != core::mem::size_of::<Response>()
        || response.magic != kMagic
        || response.version != kVersion
        || response.request_id == 0
        || response.request_id != request.request_id
        || response.logon_session_id != request.logon_session_id
    {
        return Err(win32_error(kWin32ErrorInvalidData));
    }
    Ok(())
}

/// A password returned by a kPrepare transaction. Fixed capacity, no
/// Copy/Clone, wiped on drop, backed by protected memory (vendored memsafe,
/// see secret_buffer.rs). Mirrors the C++ PreparedPipePassword.
pub struct PreparedPipePassword {
    buf: crate::secret_buffer::WindowsSecret<{ kPasswordCapacity * 2 }>,
    len: usize,
}

impl PreparedPipePassword {
    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Run `f` with a UTF-16LE read view of the password directly into
    /// protected memory; no plain `Vec<u16>` copy is produced.
    pub fn with_password<F, R>(&mut self, f: F) -> Result<R, crate::secret_buffer::SecretError>
    where
        F: FnOnce(&[u16]) -> R,
    {
        self.buf.with_u16_slice(f)
    }
}

/// Client for one pipe transaction. Stateless.
pub struct PipeClient;

/// Closes a raw handle on drop; ownership of the connected pipe stays inside
/// `transact_on`.
struct PipeGuard(HANDLE);

impl Drop for PipeGuard {
    fn drop(&mut self) {
        if !self.0.is_invalid() {
            // SAFETY: handle was created by CreateFileW and closed exactly once.
            unsafe {
                let _ = CloseHandle(self.0);
            }
        }
    }
}

/// Manual-reset event guard for overlapped waits.
struct EventGuard(HANDLE);

impl EventGuard {
    fn new() -> Result<Self, Error> {
        // SAFETY: no security attributes, manual-reset, initially unset.
        unsafe { CreateEventW(None, true, false, PCWSTR::null()) }
            .map(EventGuard)
            .map_err(|_| win32_error(kWin32ErrorServiceNotActive))
    }
}

impl Drop for EventGuard {
    fn drop(&mut self) {
        if !self.0.is_invalid() {
            // SAFETY: handle was created by CreateEventW and closed exactly once.
            unsafe {
                let _ = CloseHandle(self.0);
            }
        }
    }
}

/// Time left until `deadline_ms` (absolute `GetTickCount64` domain), capped
/// for the Win32 32-bit wait argument.
fn remaining_wait_ms(now_ms: u64, deadline_ms: u64) -> u32 {
    deadline_ms.saturating_sub(now_ms).min(u32::MAX as u64) as u32
}

impl PipeClient {
    /// Ask the broker to run its trusted recognition agent, match the probe
    /// against the SYSTEM-owned profile store, and atomically consume the
    /// one-time logon secret. The service rejects the old unauthenticated
    /// kPrepare operation.
    ///
    /// Manual path: no abort event, generous overall deadline (the service
    /// bounds the recognition work itself).
    pub fn prepare(
        &self,
        sid: &[u16],
        request_id: u64,
        logon_session_id: u32,
    ) -> Result<PreparedPipePassword, Error> {
        let deadline = now_tick() + kManualDeadlineMs;
        self.prepare_with_limits(sid, request_id, logon_session_id, None, deadline)
    }

    /// `prepare` with caller-owned cancellation and deadline. `abort` is a
    /// manual-reset event; setting it cancels the in-flight transaction with
    /// `CancelIoEx` and fails with ERROR_OPERATION_ABORTED (win32 995).
    /// `deadline_ms` is absolute in `GetTickCount64` domain.
    pub fn prepare_with_limits(
        &self,
        sid: &[u16],
        request_id: u64,
        logon_session_id: u32,
        abort: Option<HANDLE>,
        deadline_ms: u64,
    ) -> Result<PreparedPipePassword, Error> {
        self.prepare_on(
            kPipeName,
            sid,
            request_id,
            logon_session_id,
            abort,
            deadline_ms,
        )
    }

    /// `prepare_with_limits` against an explicit pipe name, so tests can
    /// exercise the full prepare path against an in-process fake server.
    fn prepare_on(
        &self,
        pipe_name: PCWSTR,
        sid: &[u16],
        request_id: u64,
        logon_session_id: u32,
        abort: Option<HANDLE>,
        deadline_ms: u64,
    ) -> Result<PreparedPipePassword, Error> {
        let mut request = Request::new(
            kOperationAuthenticateAndPrepare,
            request_id,
            logon_session_id,
        );
        if !copy_fixed(sid, &mut request.sid) {
            secure_clear(&mut request.password);
            return Err(win32_error(0x57)); // E_INVALIDARG
        }
        let mut response = Response {
            magic: 0,
            version: 0,
            reserved: 0,
            request_id: 0,
            logon_session_id: 0,
            status: 0,
            password_length: 0,
            password: [0; kPasswordCapacity],
            payload_length: 0,
            payload: [0; kPayloadCapacity],
        };
        let result = transact_on(pipe_name, &mut request, &mut response, abort, deadline_ms);
        if result.is_err() {
            // Wipe the password we sent (usually empty) and any partial reply.
            request.clear_password();
            response.clear_password();
            let err = result.err().unwrap();
            return Err(err);
        }
        let status = response.status;
        if status != kStatusOk {
            request.clear_password();
            let err = status_to_hresult(status);
            response.clear_password();
            return Err(err);
        }
        // Validate the one-time secret payload.
        let len = response.password_length as usize;
        if len == 0 || len >= kPasswordCapacity || response.password[len] != 0 {
            request.clear_password();
            response.clear_password();
            return Err(win32_error(kWin32ErrorInvalidData));
        }
        let mut out = PreparedPipePassword {
            buf: match crate::secret_buffer::WindowsSecret::new() {
                Ok(b) => b,
                Err(_) => {
                    request.clear_password();
                    response.clear_password();
                    return Err(win32_error(kWin32ErrorServiceNotActive));
                }
            },
            len: 0,
        };
        out.buf
            .with_u16_slice_mut(|dst| {
                dst[..len].copy_from_slice(&response.password[..len]);
            })
            .map_err(|_| {
                request.clear_password();
                response.clear_password();
                win32_error(kWin32ErrorServiceNotActive)
            })?;
        out.buf.set_len_units(len).map_err(|_| {
            request.clear_password();
            response.clear_password();
            win32_error(kWin32ErrorInvalidData)
        })?;
        out.len = len;
        request.clear_password();
        response.clear_password();
        Ok(out)
    }

    /// Tell the service a prepared secret must no longer be used
    /// (e.g. after a failed logon attempt).
    pub fn mark_stale(
        &self,
        sid: &[u16],
        request_id: u64,
        logon_session_id: u32,
    ) -> Result<(), Error> {
        let mut request = Request::new(kOperationMarkStale, request_id, logon_session_id);
        if !copy_fixed(sid, &mut request.sid) {
            secure_clear(&mut request.password);
            return Err(win32_error(0x57));
        }
        let mut response = Response {
            magic: 0,
            version: 0,
            reserved: 0,
            request_id: 0,
            logon_session_id: 0,
            status: 0,
            password_length: 0,
            password: [0; kPasswordCapacity],
            payload_length: 0,
            payload: [0; kPayloadCapacity],
        };
        let result = transact_on(
            kPipeName,
            &mut request,
            &mut response,
            None,
            now_tick() + kManualDeadlineMs,
        );
        request.clear_password();
        response.clear_password();
        result?;
        if response.status != kStatusOk {
            return Err(status_to_hresult(response.status));
        }
        Ok(())
    }
}

/// Monotonic milliseconds in the same domain as the runtime clock.
pub fn now_tick() -> u64 {
    // SAFETY: GetTickCount64 has no parameters and cannot fail.
    unsafe { GetTickCount64() }
}

/// Connect to a message-mode pipe instance, bounded by `deadline_ms` and
/// observing `abort` between wait slices.
fn connect_pipe(
    pipe_name: PCWSTR,
    abort: Option<HANDLE>,
    deadline_ms: u64,
) -> Result<PipeGuard, Error> {
    loop {
        // SAFETY: pipe_name is a valid NUL-terminated wide string; the handle
        // is closed by PipeGuard on every path.
        let handle = unsafe {
            CreateFileW(
                pipe_name,
                (GENERIC_READ | GENERIC_WRITE).0,
                FILE_SHARE_NONE,
                None,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                None,
            )
        };
        match handle {
            Ok(handle) => {
                let mode = PIPE_READMODE_MESSAGE;
                // SAFETY: handle is a connected pipe; mode points to a valid
                // NAMED_PIPE_MODE value; the optional out-params are null.
                unsafe { SetNamedPipeHandleState(handle, Some(&mode), None, None) }?;
                return Ok(PipeGuard(handle));
            }
            Err(error) => {
                let code = error.code().0 as u32 & 0xffff;
                if code != kWin32ErrorPipeBusy {
                    return Err(error);
                }
            }
        }
        let now = now_tick();
        let remaining = remaining_wait_ms(now, deadline_ms);
        if remaining == 0 {
            return Err(win32_error(kWin32ErrorSemTimeout));
        }
        if let Some(abort) = abort {
            // SAFETY: abort is a valid event handle; a zero timeout only polls.
            if unsafe { WaitForSingleObject(abort, 0) }.0 == WAIT_OBJECT_0.0 {
                return Err(win32_error(kWin32ErrorOperationAborted));
            }
        }
        // SAFETY: pipe_name is valid; the slice keeps the wait responsive.
        let waited = unsafe { WaitNamedPipeW(pipe_name, remaining.min(kConnectWaitSliceMs)) };
        if !waited.as_bool() && now_tick() >= deadline_ms {
            return Err(win32_error(kWin32ErrorSemTimeout));
        }
    }
}

/// Wait for a pending overlapped operation on `(io completion, abort,
/// deadline)` and reclaim the OVERLAPPED. Returns the transferred byte count
/// on completion; aborted waits fail with 995, expired deadlines with 1460.
fn wait_io(
    pipe: HANDLE,
    overlapped: &mut OVERLAPPED,
    io_event: HANDLE,
    abort: Option<HANDLE>,
    deadline_ms: u64,
) -> Result<u32, Error> {
    let timeout = remaining_wait_ms(now_tick(), deadline_ms);
    let wait = match abort {
        Some(abort) => {
            // SAFETY: both handles are valid; two-entry array, no wait-all.
            unsafe { WaitForMultipleObjects(&[io_event, abort], false, timeout) }
        }
        None => {
            // SAFETY: valid event handle.
            unsafe { WaitForSingleObject(io_event, timeout) }
        }
    };
    let mut transferred = 0u32;
    let trigger = if wait.0 == WAIT_OBJECT_0.0 {
        // Index 0 is the io event in both wait shapes.
        // SAFETY: the OVERLAPPED is owned by the caller and complete.
        return match unsafe {
            GetOverlappedResult(
                pipe,
                overlapped as *const OVERLAPPED,
                &mut transferred,
                false,
            )
        } {
            Ok(()) => Ok(transferred),
            Err(error) => Err(error),
        };
    } else if wait.0 == WAIT_TIMEOUT.0 {
        kWin32ErrorTimeout
    } else if abort.is_some() && wait.0 == WAIT_OBJECT_0.0 + 1 {
        kWin32ErrorOperationAborted
    } else {
        unsafe { GetLastError().0 }
    };
    // CancelIoEx is thread-safe and needs no handle to the waiting thread.
    // The blocking GetOverlappedResult then reclaims the OVERLAPPED before
    // the caller drops it; after a successful cancel it returns quickly with
    // ERROR_OPERATION_ABORTED, which we swallow in favor of the trigger code.
    // SAFETY: pipe is a valid overlapped handle; overlapped belongs to us.
    unsafe {
        let _ = CancelIoEx(pipe, Some(overlapped as *mut OVERLAPPED));
        let _ = GetOverlappedResult(
            pipe,
            overlapped as *const OVERLAPPED,
            &mut transferred,
            true,
        );
    }
    Err(win32_error(trigger))
}

/// One overlapped write. Returns the transferred byte count on completion.
fn overlapped_write(
    pipe: HANDLE,
    buffer: &[u8],
    abort: Option<HANDLE>,
    deadline_ms: u64,
) -> Result<u32, Error> {
    let io_event = EventGuard::new()?;
    let mut overlapped = OVERLAPPED {
        hEvent: io_event.0,
        ..Default::default()
    };
    let mut transferred = 0u32;
    // SAFETY: buffer is valid for the duration of the call; the OVERLAPPED is
    // not reused after the wait completes.
    let immediate = unsafe {
        WriteFile(
            pipe,
            Some(buffer),
            Some(&mut transferred),
            Some(&mut overlapped),
        )
    };
    if immediate.is_ok() {
        return Ok(transferred);
    }
    let code = unsafe { GetLastError().0 };
    if code != kWin32ErrorIoPending {
        return Err(win32_error(code));
    }
    wait_io(pipe, &mut overlapped, io_event.0, abort, deadline_ms)
}

/// One overlapped read. Returns the transferred byte count on completion.
fn overlapped_read(
    pipe: HANDLE,
    buffer: &mut [u8],
    abort: Option<HANDLE>,
    deadline_ms: u64,
) -> Result<u32, Error> {
    let io_event = EventGuard::new()?;
    let mut overlapped = OVERLAPPED {
        hEvent: io_event.0,
        ..Default::default()
    };
    let mut transferred = 0u32;
    // SAFETY: buffer is valid for the duration of the call; the OVERLAPPED is
    // not reused after the wait completes.
    let immediate = unsafe {
        ReadFile(
            pipe,
            Some(buffer),
            Some(&mut transferred),
            Some(&mut overlapped),
        )
    };
    if immediate.is_ok() {
        return Ok(transferred);
    }
    let code = unsafe { GetLastError().0 };
    if code != kWin32ErrorIoPending {
        return Err(win32_error(code));
    }
    wait_io(pipe, &mut overlapped, io_event.0, abort, deadline_ms)
}

/// One message-mode transaction with cancellation and an absolute deadline.
fn transact_on(
    pipe_name: PCWSTR,
    request: &mut Request,
    response: &mut Response,
    abort: Option<HANDLE>,
    deadline_ms: u64,
) -> Result<(), Error> {
    let pipe = connect_pipe(pipe_name, abort, deadline_ms)?;
    if let Err(error) = overlapped_write(pipe.0, request.request_bytes(), abort, deadline_ms) {
        crate::log::cp_log(&format!(
            "pipe write FAILED code={:#x}",
            error.code().0 as u32 & 0xffff
        ));
        return Err(error);
    }
    let bytes_read =
        match overlapped_read(pipe.0, response.response_bytes_mut(), abort, deadline_ms) {
            Ok(bytes) => bytes,
            Err(error) => {
                crate::log::cp_log(&format!(
                    "pipe read FAILED code={:#x}",
                    error.code().0 as u32 & 0xffff
                ));
                return Err(error);
            }
        };
    crate::log::cp_log(&format!(
        "pipe transact OK bytes_read={} status={}",
        bytes_read, response.status
    ));
    // Every response must round-trip the request identity before the
    // caller may consume its payload (magic/version/request_id/session).
    validate_response(response, request, bytes_read)?;
    Ok(())
}

/// Unique-enough request id for the service replay cache. A naive per-process
/// counter starting at 1 collides with ids the service has already seen (it
/// keeps a process-lifetime cache), so seed with the current time and PID and
/// then bump a process-local counter.
pub fn next_request_id() -> u64 {
    use core::sync::atomic::{AtomicU64, Ordering};
    static NEXT_REQUEST_ID: AtomicU64 = AtomicU64::new(0);
    let millis = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);
    // SAFETY: GetCurrentProcessId has no parameters and cannot fail.
    let pid = unsafe { windows::Win32::System::Threading::GetCurrentProcessId() } as u64;
    let base = (millis << 24) ^ (pid << 8) ^ (millis & 0xff);
    let base = if base == 0 { 1 } else { base };
    base + NEXT_REQUEST_ID.fetch_add(1, Ordering::Relaxed)
}

/// LogonUI's session id (the console session on the lock screen).
pub fn current_session_id() -> u32 {
    let mut session_id = 0u32;
    // SAFETY: both arguments are valid; a failure leaves session_id at 0 and
    // the service falls back to the active console session.
    let _ = unsafe {
        windows::Win32::System::RemoteDesktop::ProcessIdToSessionId(
            windows::Win32::System::Threading::GetCurrentProcessId(),
            &mut session_id,
        )
    };
    session_id
}

/// Current process owner SID as a string, or Err on failure.
pub fn current_user_sid() -> Result<String, Error> {
    let process = unsafe { GetCurrentProcess() };
    let mut token: windows::Win32::Foundation::HANDLE =
        windows::Win32::Foundation::HANDLE(core::ptr::null_mut());
    unsafe { OpenProcessToken(process, TOKEN_QUERY, &mut token)? };
    let mut buf = [0u8; 256];
    let result = unsafe {
        GetTokenInformation(
            token,
            TokenUser,
            Some(buf.as_mut_ptr().cast()),
            buf.len() as u32,
            &mut 0u32,
        )
    };
    if let Err(error) = result {
        unsafe {
            let _ = windows::Win32::Foundation::CloseHandle(token);
        }
        return Err(error);
    }
    // Safety: GetTokenInformation succeeded with a buffer large enough for
    // TOKEN_USER (the SID itself is at most 68 bytes).
    let token_user = unsafe { &*(buf.as_ptr().cast::<TOKEN_USER>()) };
    let mut sid_string: PWSTR = PWSTR::null();
    let conv = unsafe { ConvertSidToStringSidW(token_user.User.Sid, &mut sid_string) };
    if let Err(error) = conv {
        unsafe {
            let _ = windows::Win32::Foundation::CloseHandle(token);
        }
        return Err(error);
    }
    let text = unsafe {
        // Safety: sid_string is a null-terminated string allocated by the
        // system, valid until LocalFree.
        let len = (0..usize::MAX)
            .find(|&i| *sid_string.as_ptr().add(i) == 0)
            .unwrap_or(0);
        String::from_utf16_lossy(core::slice::from_raw_parts(sid_string.as_ptr(), len))
    };
    unsafe {
        windows::Win32::Foundation::LocalFree(Some(windows::Win32::Foundation::HLOCAL(
            sid_string.as_ptr() as *mut core::ffi::c_void,
        )));
        let _ = windows::Win32::Foundation::CloseHandle(token);
    }
    Ok(text)
}

#[cfg(all(test, windows))]
mod tests {
    use super::*;

    fn request_fixture() -> Request {
        Request::new(kOperationPrepare, 42, 7)
    }

    #[test]
    fn request_layout() {
        assert_eq!(core::mem::size_of::<Request>(), 51608);
        let r = request_fixture();
        let base = &r as *const Request as usize;
        let magic = &r.magic as *const u32 as usize - base;
        assert_eq!(magic, 0);
        assert_eq!(&r.version as *const u16 as usize - base, 4);
        assert_eq!(&r.operation as *const u16 as usize - base, 6);
        assert_eq!(&r.request_id as *const u64 as usize - base, 8);
        assert_eq!(&r.logon_session_id as *const u32 as usize - base, 16);
        assert_eq!(&r.account_kind as *const u32 as usize - base, 20);
        assert_eq!(&r.sid as *const [u16; 185] as usize - base, 24);
        assert_eq!(
            &r.canonical_username as *const [u16; 513] as usize - base,
            394
        );
        assert_eq!(&r.password as *const [u16; 513] as usize - base, 1420);
        assert_eq!(&r.payload_length as *const u32 as usize - base, 2448);
        assert_eq!(
            &r.payload as *const [u8; kPayloadCapacity] as usize - base,
            2452
        );
    }

    #[test]
    fn response_layout() {
        assert_eq!(core::mem::size_of::<Response>(), 50216);
        let mut r = Response {
            magic: 0,
            version: 0,
            reserved: 0,
            request_id: 0,
            logon_session_id: 0,
            status: 0,
            password_length: 0,
            password: [0; 513],
            payload_length: 0,
            payload: [0; kPayloadCapacity],
        };
        let base = &mut r as *mut Response as usize;
        assert_eq!(&mut r.magic as *mut u32 as usize - base, 0);
        assert_eq!(&mut r.version as *mut u16 as usize - base, 4);
        assert_eq!(&mut r.reserved as *mut u16 as usize - base, 6);
        assert_eq!(&mut r.request_id as *mut u64 as usize - base, 8);
        assert_eq!(&mut r.logon_session_id as *mut u32 as usize - base, 16);
        assert_eq!(&mut r.status as *mut u32 as usize - base, 20);
        assert_eq!(&mut r.password_length as *mut u32 as usize - base, 24);
        assert_eq!(&mut r.password as *mut [u16; 513] as usize - base, 28);
        assert_eq!(&mut r.payload_length as *mut u32 as usize - base, 1056);
        assert_eq!(
            &mut r.payload as *mut [u8; kPayloadCapacity] as usize - base,
            1060
        );
    }

    #[test]
    fn request_fields_filled() {
        let r = request_fixture();
        assert_eq!(r.magic, kMagic);
        assert_eq!(r.version, kVersion);
        assert_eq!(r.operation, kOperationPrepare);
        assert_eq!(r.request_id, 42);
        assert_eq!(r.logon_session_id, 7);
        assert!(r.sid.iter().all(|&u| u == 0));
    }

    #[test]
    fn response_validation() {
        let req = request_fixture();
        let mut good = Response {
            magic: kMagic,
            version: kVersion,
            reserved: 0,
            request_id: 42,
            logon_session_id: 7,
            status: kStatusOk,
            password_length: 0,
            password: [0; 513],
            payload_length: 0,
            payload: [0; kPayloadCapacity],
        };
        assert!(validate_response(&good, &req, core::mem::size_of::<Response>() as u32).is_ok());

        good.magic = 0;
        assert!(validate_response(&good, &req, core::mem::size_of::<Response>() as u32).is_err());
        good.magic = kMagic;

        good.version = 0;
        assert!(validate_response(&good, &req, core::mem::size_of::<Response>() as u32).is_err());
        good.version = kVersion;

        good.request_id = 0;
        assert!(validate_response(&good, &req, core::mem::size_of::<Response>() as u32).is_err());
        good.request_id = 43;
        assert!(validate_response(&good, &req, core::mem::size_of::<Response>() as u32).is_err());
        good.request_id = 42;

        good.logon_session_id = 8;
        assert!(validate_response(&good, &req, core::mem::size_of::<Response>() as u32).is_err());
        good.logon_session_id = 7;

        assert!(validate_response(&good, &req, 0).is_err());
        assert!(
            validate_response(&good, &req, core::mem::size_of::<Response>() as u32 + 1).is_err()
        );
    }

    #[test]
    fn status_mapping_table() {
        assert_eq!(status_to_hresult(kStatusOk).code().0, 0);
        assert_eq!(
            status_to_hresult(kStatusInvalidRequest).code().0,
            0x80070057u32 as i32
        );
        assert_eq!(
            status_to_hresult(kStatusAccessDenied).code().0,
            0x80070005u32 as i32
        );
        assert_eq!(
            status_to_hresult(kStatusUnavailable).code().0,
            0x80070426u32 as i32
        ); // ERROR_SERVICE_NOT_ACTIVE=1062=0x426
        assert_eq!(
            status_to_hresult(kStatusStaleOrConsumed).code().0,
            0x80070025u32 as i32
        ); // ERROR_PASSWORD_RESTRICTION
        assert_eq!(
            status_to_hresult(kStatusCorrupt).code().0,
            0x8007000Du32 as i32
        ); // ERROR_INVALID_DATA
        assert_eq!(status_to_hresult(99).code().0, 0x8007000Du32 as i32);
    }

    #[test]
    fn copy_fixed_bounds() {
        let mut dst = [0u16; 185];
        assert!(!copy_fixed(&[], &mut dst));
        assert!(!copy_fixed(&[1u16; 185], &mut dst));
        assert!(!copy_fixed(&[1u16; 186], &mut dst));
        assert!(copy_fixed(&[1u16; 184], &mut dst));
        assert_eq!(dst[183], 1);
        assert_eq!(dst[184], 0);
        let src = "S-1-5-21-1-2-3-4".encode_utf16().collect::<Vec<_>>();
        assert!(copy_fixed(&src, &mut dst));
        assert!(dst[..src.len()].iter().zip(src.iter()).all(|(a, b)| a == b));
        assert_eq!(dst[src.len()], 0);
    }

    #[test]
    fn prepared_password_drop_wipes() {
        // Write 200 units, then drop. WindowsSecret::drop zeroizes the
        // protected page before memsafe releases it; the test asserts the
        // write/read path and a clean drop (no panic).
        let mut pp = PreparedPipePassword {
            buf: crate::secret_buffer::WindowsSecret::new().unwrap(),
            len: 0,
        };
        pp.buf
            .with_u16_slice_mut(|dst| {
                for (i, unit) in dst.iter_mut().enumerate().take(200) {
                    *unit = (i as u16).wrapping_add(1);
                }
            })
            .unwrap();
        pp.buf.set_len_units(200).unwrap();
        pp.len = 200;
        pp.buf
            .with_u16_slice(|units| {
                assert_eq!(units.len(), 200);
                assert_eq!(units[0], 1);
                assert_eq!(units[199], 200);
            })
            .unwrap();
        drop(pp);
    }

    #[test]
    fn secure_clear_wipes() {
        let mut buf = [0xABu16; 100];
        secure_clear(&mut buf);
        assert!(buf.iter().all(|&u| u == 0));
    }

    #[test]
    fn transact_without_server_fails() {
        // No service is running in the test environment; the call must fail
        // with a Win32 error (ERROR_FILE_NOT_FOUND or ERROR_PIPE_BUSY etc.).
        let client = PipeClient;
        let sid = "S-1-5-21-1-2-3-4".encode_utf16().collect::<Vec<_>>();
        let result = client.prepare(&sid, 99, 1);
        assert!(result.is_err());
    }

    // ---- overlapped transaction tests against an in-process fake server ----

    use std::thread;
    use std::time::Duration;

    fn wide_name(tag: &str) -> (Vec<u16>, PCWSTR) {
        let text = format!(r"\\.\pipe\Smile2UnlockTest.{tag}");
        let mut wide: Vec<u16> = text.encode_utf16().collect();
        wide.push(0);
        let ptr = PCWSTR(wide.as_ptr());
        (wide, ptr)
    }

    /// Serve exactly one transaction: connect, read the request, sleep
    /// `delay_ms`, echo a valid kStatusOk response, disconnect. Takes the
    /// NUL-terminated wide name by value: PCWSTR is not Send, so the pointer
    /// is rebuilt inside the server thread.
    fn spawn_echo_server(
        pipe_name_wide: Vec<u16>,
        delay_ms: u64,
        seen_request_ids: std::sync::Arc<std::sync::Mutex<Vec<u64>>>,
    ) -> thread::JoinHandle<()> {
        use windows::Win32::Storage::FileSystem::{PIPE_ACCESS_DUPLEX, ReadFile, WriteFile};
        use windows::Win32::System::Pipes::{
            ConnectNamedPipe, CreateNamedPipeW, DisconnectNamedPipe, PIPE_READMODE_MESSAGE,
            PIPE_TYPE_MESSAGE, PIPE_WAIT,
        };
        thread::spawn(move || {
            let pipe_name = PCWSTR(pipe_name_wide.as_ptr());
            // SAFETY: name is NUL-terminated; one synchronous server instance.
            let server = unsafe {
                CreateNamedPipeW(
                    pipe_name,
                    PIPE_ACCESS_DUPLEX,
                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                    1,
                    core::mem::size_of::<Response>() as u32,
                    core::mem::size_of::<Request>() as u32,
                    0,
                    None,
                )
            };
            assert!(!server.is_invalid(), "CreateNamedPipeW failed");
            // SAFETY: server handle is valid and listening. A client that
            // aborts during connect surfaces on the client side; the server
            // thread simply finishes without serving.
            let _ = unsafe { ConnectNamedPipe(server, None) };
            let mut request = Request {
                magic: 0,
                version: 0,
                operation: 0,
                request_id: 0,
                logon_session_id: 0,
                account_kind: 0,
                sid: [0; kSidCapacity],
                canonical_username: [0; kUsernameCapacity],
                password: [0; kPasswordCapacity],
                payload_length: 0,
                payload: [0; kPayloadCapacity],
            };
            let mut read_bytes = 0u32;
            // SAFETY: the byte view of a properly aligned Request is valid
            // for the full read; casting the struct (not a u8 array) keeps
            // the u64 fields aligned.
            let request_bytes = unsafe {
                core::slice::from_raw_parts_mut(
                    (&mut request as *mut Request).cast::<u8>(),
                    core::mem::size_of::<Request>(),
                )
            };
            let read =
                unsafe { ReadFile(server, Some(request_bytes), Some(&mut read_bytes), None) };
            if read.is_ok() && read_bytes as usize == core::mem::size_of::<Request>() {
                seen_request_ids.lock().unwrap().push(request.request_id);
                if delay_ms > 0 {
                    thread::sleep(Duration::from_millis(delay_ms));
                }
                let mut response = Response {
                    magic: kMagic,
                    version: kVersion,
                    reserved: 0,
                    request_id: request.request_id,
                    logon_session_id: request.logon_session_id,
                    status: kStatusOk,
                    password_length: 3,
                    password: [0; kPasswordCapacity],
                    payload_length: 0,
                    payload: [0; kPayloadCapacity],
                };
                response.password[0] = b'p' as u16;
                response.password[1] = b'w' as u16;
                response.password[2] = 0;
                // SAFETY: response buffer is valid for the full write.
                let response_bytes = unsafe {
                    core::slice::from_raw_parts(
                        (&response as *const Response).cast::<u8>(),
                        core::mem::size_of::<Response>(),
                    )
                };
                let mut written = 0u32;
                let write =
                    unsafe { WriteFile(server, Some(response_bytes), Some(&mut written), None) };
                // A client that aborted or hit its deadline has closed its
                // end; the delayed echo failing to deliver is expected.
                let _ = write;
            }
            // SAFETY: server handle is valid.
            unsafe {
                let _ = DisconnectNamedPipe(server);
                let _ = CloseHandle(server);
            }
        })
    }

    fn abort_event() -> HANDLE {
        // SAFETY: manual-reset, initially unset, unnamed event.
        unsafe { CreateEventW(None, true, false, PCWSTR::null()) }.expect("CreateEventW")
    }

    /// Block until the server thread has created its pipe instance, so the
    /// client's first CreateFileW does not race instance creation.
    fn wait_pipe_ready(name: PCWSTR) {
        use windows::Win32::System::Pipes::WaitNamedPipeW;
        let deadline = now_tick() + 2_000;
        while now_tick() < deadline {
            // SAFETY: name is NUL-terminated; a zero timeout only polls.
            if unsafe { WaitNamedPipeW(name, 0) }.as_bool() {
                return;
            }
            thread::sleep(Duration::from_millis(10));
        }
        panic!("test pipe was not created in time");
    }

    fn win32_code(error: &Error) -> u32 {
        error.code().0 as u32 & 0xffff
    }

    #[test]
    fn overlapped_transaction_completes() {
        let (name_storage, name) = wide_name("happy");
        let seen = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
        let server = spawn_echo_server(name_storage.clone(), 0, std::sync::Arc::clone(&seen));
        // Give the server a moment to create its instance; a missed race only
        // costs one connect retry slice, so poll instead of sleeping long.
        let deadline = now_tick() + 2_000;
        let mut request = Request::new(kOperationAuthenticateAndPrepare, 4242, 5);
        let sid = "S-1-5-21-1-2-3-4".encode_utf16().collect::<Vec<_>>();
        assert!(copy_fixed(&sid, &mut request.sid));
        let mut response = Response {
            magic: 0,
            version: 0,
            reserved: 0,
            request_id: 0,
            logon_session_id: 0,
            status: 0,
            password_length: 0,
            password: [0; kPasswordCapacity],
            payload_length: 0,
            payload: [0; kPayloadCapacity],
        };
        let mut result = Err(win32_error(kWin32ErrorSemTimeout));
        while now_tick() < deadline {
            let mut request_copy = request;
            let mut response_copy = Response {
                magic: 0,
                version: 0,
                reserved: 0,
                request_id: 0,
                logon_session_id: 0,
                status: 0,
                password_length: 0,
                password: [0; kPasswordCapacity],
                payload_length: 0,
                payload: [0; kPayloadCapacity],
            };
            result = transact_on(name, &mut request_copy, &mut response_copy, None, deadline);
            response = response_copy;
            request_copy.clear_password();
            if result.is_ok() {
                break;
            }
            let code = result.as_ref().map(|_| 0).unwrap_or_else(win32_code);
            // The server thread may not have created its instance yet, so
            // FILE_NOT_FOUND is a retryable race like a busy pipe.
            if code != kWin32ErrorPipeBusy && code != kWin32ErrorSemTimeout && code != 2 {
                break;
            }
            thread::sleep(Duration::from_millis(20));
        }
        assert!(result.is_ok(), "transact failed: {:?}", result.err());
        assert_eq!(response.request_id, 4242);
        assert_eq!(response.logon_session_id, 5);
        assert_eq!(response.status, kStatusOk);
        assert_eq!(response.password_length, 3);
        server.join().unwrap();
        assert_eq!(*seen.lock().unwrap(), vec![4242]);
        drop(name_storage);
    }

    #[test]
    fn overlapped_abort_returns_promptly() {
        let (name_storage, name) = wide_name("abort");
        let seen = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
        // Server stalls 5 s before responding.
        let server = spawn_echo_server(name_storage.clone(), 5_000, seen);
        wait_pipe_ready(name);
        let abort = abort_event();
        let client = PipeClient;
        let sid = "S-1-5-21-1-2-3-4".encode_utf16().collect::<Vec<_>>();
        // HANDLE is a raw pointer and thus not Send; pass it as a usize and
        // rebuild it inside the setter thread.
        let abort_raw = abort.0 as usize;
        let starter = thread::spawn(move || {
            use windows::Win32::System::Threading::SetEvent;
            thread::sleep(Duration::from_millis(150));
            // SAFETY: rebuilt from a live event handle.
            unsafe {
                let _ = SetEvent(HANDLE(abort_raw as *mut core::ffi::c_void));
            }
        });
        let started = std::time::Instant::now();
        let result = client.prepare_on(name, &sid, 7, 3, Some(abort), now_tick() + 30_000);
        let elapsed = started.elapsed();
        let _ = unsafe { CloseHandle(abort) };
        let error = match result {
            Ok(_) => panic!("aborted transaction must fail"),
            Err(error) => error,
        };
        assert_eq!(win32_code(&error), kWin32ErrorOperationAborted);
        assert!(
            elapsed < Duration::from_secs(3),
            "abort took {elapsed:?}, cancellation was not prompt"
        );
        starter.join().unwrap();
        // The server eventually finishes its delayed response; join so the
        // test process exits cleanly.
        server.join().unwrap();
        drop(name_storage);
    }

    #[test]
    fn overlapped_deadline_returns_promptly() {
        let (name_storage, name) = wide_name("deadline");
        let seen = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
        let server = spawn_echo_server(name_storage.clone(), 5_000, seen);
        wait_pipe_ready(name);
        let client = PipeClient;
        let sid = "S-1-5-21-1-2-3-4".encode_utf16().collect::<Vec<_>>();
        let started = std::time::Instant::now();
        let result = client.prepare_on(name, &sid, 8, 3, None, now_tick() + 300);
        let elapsed = started.elapsed();
        let error = match result {
            Ok(_) => panic!("expired deadline must fail"),
            Err(error) => error,
        };
        assert_eq!(win32_code(&error), kWin32ErrorTimeout);
        assert!(
            elapsed < Duration::from_secs(3),
            "deadline enforcement took {elapsed:?}"
        );
        server.join().unwrap();
        drop(name_storage);
    }
}
