// Wire constants mirror the C++ protocol-header naming (kMagic, kSidCapacity,
// ...) so the mapping to common/windows/logon_secret_protocol.h stays
// greppable; the casing lint is disabled for this module.
#![allow(non_upper_case_globals)]

//! Named-pipe client for the LocalSystem Smile2Unlock logon-secret service.
//!
//! Wire format: fixed-size binary structs shared with the C++ service
//! (common/windows/logon_secret_protocol.h). One CallNamedPipeW transaction
//! per request; the pipe is message-mode, 3000 ms timeout.
//!
//! Security posture:
//! - kPrepare / kMarkStale are only honored by the service when the caller
//!   is LocalSystem, so this client is only meaningful from LogonUI.
//! - Passwords are held in fixed-capacity buffers and wiped with volatile
//!   stores on every path (success, failure, drop).

use windows::Win32::Foundation::GetLastError;
use windows::Win32::System::Pipes::CallNamedPipeW;
use windows::Win32::Security::{
    GetTokenInformation, TokenUser, TOKEN_QUERY, TOKEN_USER,
};
use windows::Win32::Security::Authorization::ConvertSidToStringSidW;
use windows::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};
use windows_core::{Error, HRESULT, PCWSTR, PWSTR};

/// HRESULT_FROM_WIN32(code): error codes with the FACILITY_WIN32 severity bit.
/// windows-core 0.62 split `Error` into windows-result 0.4.1, which exposes
/// only `Error::from_hresult`; this restores the familiar helper.
pub fn win32_error(code: u32) -> Error {
    Error::from_hresult(HRESULT((0x8007_0000u32 | (code & 0xffff)) as i32))
}

pub const kMagic: u32 = 0x5332_5350; // "S2SP"
pub const kVersion: u16 = 1;
pub const kPipeName: PCWSTR = windows_core::w!(r"\\.\pipe\Smile2Unlock.LogonSecret.v1");

pub const kOperationPrepare: u16 = 1;
pub const kOperationMarkStale: u16 = 2;
#[allow(dead_code)] // Phase 5 (store/clear flow)
pub const kOperationStore: u16 = 3;
#[allow(dead_code)] // Phase 5 (store/clear flow)
pub const kOperationClear: u16 = 4;

pub const kStatusOk: u32 = 0;
pub const kStatusInvalidRequest: u32 = 1;
pub const kStatusAccessDenied: u32 = 2;
pub const kStatusUnavailable: u32 = 3;
pub const kStatusStaleOrConsumed: u32 = 4;
pub const kStatusCorrupt: u32 = 5;

pub const kSidCapacity: usize = 185;
pub const kUsernameCapacity: usize = 513;
pub const kPasswordCapacity: usize = 513;

const kWin32ErrorInvalidData: u32 = 13;
const kWin32ErrorPasswordRestriction: u32 = 37;
const kWin32ErrorServiceNotActive: u32 = 1062;

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
        self.password_length = 0;
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
        kStatusUnavailable => win32_error(kWin32ErrorServiceNotActive),
        _ => win32_error(kWin32ErrorInvalidData),
    }
}

/// Validate a received response against the request and the protocol.
/// `bytes_read` is the CallNamedPipeW reported byte count; it must equal the
/// full response size (message-mode pipe guarantees atomic messages).
pub fn validate_response(response: &Response, request: &Request, bytes_read: u32) -> Result<(), Error> {
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
/// Copy/Clone, wiped on drop. Mirrors the C++ PreparedPipePassword.
pub struct PreparedPipePassword {
    buf: [u16; kPasswordCapacity],
    len: usize,
}

impl PreparedPipePassword {
    pub fn len(&self) -> usize {
        self.len
    }

    pub fn as_u16_slice(&self) -> &[u16] {
        &self.buf[..self.len]
    }
}

impl Drop for PreparedPipePassword {
    fn drop(&mut self) {
        secure_clear(&mut self.buf);
        self.len = 0;
    }
}

/// Client for one synchronous pipe transaction. Stateless.
pub struct PipeClient;

impl PipeClient {
    /// Request the service to prepare a one-time logon secret for `sid`.
    /// The returned password is single-use; do not retransmit after a failed
    /// ReportResult (the credential layer marks it stale instead).
    pub fn prepare(
        &self,
        sid: &[u16],
        request_id: u64,
        logon_session_id: u32,
    ) -> Result<PreparedPipePassword, Error> {
        let mut request = Request::new(kOperationPrepare, request_id, logon_session_id);
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
        };
        let result = self.transact(&mut request, &mut response);
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
            buf: [0; kPasswordCapacity],
            len,
        };
        out.buf[..len].copy_from_slice(&response.password[..len]);
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
        };
        let result = self.transact(&mut request, &mut response);
        request.clear_password();
        response.clear_password();
        result?;
        if response.status != kStatusOk {
            return Err(status_to_hresult(response.status));
        }
        Ok(())
    }

    /// One synchronous, message-mode transaction with a 3000 ms timeout.
    fn transact(&self, request: &mut Request, response: &mut Response) -> Result<(), Error> {
        // Safety: buffers are valid for the full duration of the blocking
        // call; sizes match the protocol structs.
        let mut bytes_read: u32 = 0;
        let ok = unsafe {
            CallNamedPipeW(
                kPipeName,
                Some(request.request_bytes().as_ptr().cast()),
                core::mem::size_of::<Request>() as u32,
                Some(response.response_bytes_mut().as_mut_ptr().cast()),
                core::mem::size_of::<Response>() as u32,
                &mut bytes_read,
                3000,
            )
        };
        if !ok.as_bool() {
            let code = unsafe { GetLastError().0 };
            return Err(win32_error(code));
        }
        // Every response must round-trip the request identity before the
        // caller may consume its payload (magic/version/request_id/session).
        validate_response(response, request, bytes_read)?;
        Ok(())
    }
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
    if result.is_err() {
        unsafe {
            let _ = windows::Win32::Foundation::CloseHandle(token);
        }
        return Err(result.unwrap_err());
    }
    // Safety: GetTokenInformation succeeded with a buffer large enough for
    // TOKEN_USER (the SID itself is at most 68 bytes).
    let token_user = unsafe { &*(buf.as_ptr().cast::<TOKEN_USER>()) };
    let mut sid_string: PWSTR = PWSTR::null();
    let conv = unsafe { ConvertSidToStringSidW(token_user.User.Sid, &mut sid_string) };
    if conv.is_err() {
        unsafe {
            let _ = windows::Win32::Foundation::CloseHandle(token);
        }
        return Err(conv.unwrap_err());
    }
    let text = unsafe {
        // Safety: sid_string is a null-terminated string allocated by the
        // system, valid until LocalFree.
        let len = (0..usize::MAX).find(|&i| *sid_string.as_ptr().add(i) == 0).unwrap_or(0);
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
        assert_eq!(core::mem::size_of::<Request>(), 2448);
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
        assert_eq!(&r.canonical_username as *const [u16; 513] as usize - base, 394);
        assert_eq!(&r.password as *const [u16; 513] as usize - base, 1420);
    }

    #[test]
    fn response_layout() {
        assert_eq!(core::mem::size_of::<Response>(), 1056);
        let mut r = Response {
            magic: 0,
            version: 0,
            reserved: 0,
            request_id: 0,
            logon_session_id: 0,
            status: 0,
            password_length: 0,
            password: [0; 513],
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
        assert!(validate_response(&good, &req, core::mem::size_of::<Response>() as u32 + 1).is_err());
    }

    #[test]
    fn status_mapping_table() {
        assert_eq!(status_to_hresult(kStatusOk).code().0, 0);
        assert_eq!(status_to_hresult(kStatusInvalidRequest).code().0, 0x80070057u32 as i32);
        assert_eq!(status_to_hresult(kStatusAccessDenied).code().0, 0x80070005u32 as i32);
        assert_eq!(status_to_hresult(kStatusUnavailable).code().0, 0x80070426u32 as i32); // ERROR_SERVICE_NOT_ACTIVE=1062=0x426
        assert_eq!(status_to_hresult(kStatusStaleOrConsumed).code().0, 0x80070025u32 as i32); // ERROR_PASSWORD_RESTRICTION
        assert_eq!(status_to_hresult(kStatusCorrupt).code().0, 0x8007000Du32 as i32); // ERROR_INVALID_DATA
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
        let pp = Box::new(PreparedPipePassword { buf: [0u16; 513], len: 0 });
        let mut pp = pp;
        for (i, unit) in pp.buf.iter_mut().enumerate().take(200) {
            *unit = (i as u16).wrapping_add(1);
        }
        pp.len = 200;
        let ptr = pp.buf.as_ptr();
        unsafe { core::ptr::drop_in_place(Box::into_raw(pp)) };
        let leaked = unsafe { core::slice::from_raw_parts(ptr, 513) };
        assert!(leaked.iter().all(|&u| u == 0));
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
}
