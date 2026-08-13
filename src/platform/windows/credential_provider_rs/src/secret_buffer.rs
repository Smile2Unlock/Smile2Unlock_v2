//! Fixed-capacity secret buffer used for the one-time logon credential
//! (UTF-16LE bytes) and, in Phase 2+, as the pipe-client transport buffer.
//!
//! Security model (docs/windows_credential_provider_rust_plan.md, memsafe
//! section):
//! - Fixed capacity: the credential is stored as bytes + length, never as a
//!   growable String/Vec and never converted to a plain `u16`/`String` copy.
//! - On Windows the pages are VirtualLock'ed and, while not in use, flipped
//!   to PAGE_NOACCESS ("sealed"). Unsealing is the only way to read.
//! - Drop zeroizes the buffer unconditionally (fail-secure: protection API
//!   failures never block erasure and never panic/unwrap).
//! - No Clone, no Debug, no Default printing of contents.
//!
//! Phase 0 ships the prototype; the production gate (stale-pointer,
//! thread-interleave, dump, lock-page tests) is tracked in the plan doc.

use core::fmt;

#[cfg(windows)]
use windows_sys::Win32::System::Memory::{
    VirtualLock, VirtualProtect, VirtualQuery, VirtualUnlock, PAGE_NOACCESS, PAGE_READWRITE,
    MEMORY_BASIC_INFORMATION,
};

/// The buffer capacity was exceeded; nothing is written and the buffer is
/// left fully zeroed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CapacityError {
    pub capacity_bytes: usize,
    pub needed_bytes: usize,
}

impl fmt::Display for CapacityError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "WindowsSecret capacity exceeded: need {} bytes, have {}",
            self.needed_bytes, self.capacity_bytes
        )
    }
}

impl core::error::Error for CapacityError {}

/// Fixed-capacity, zeroizing, optionally page-sealed byte buffer.
///
/// `N` is the byte capacity (UTF-16LE, so an even number of bytes). The
/// struct is 2-byte aligned so `as_u16_ptr` is always properly aligned for
/// the KERB / LogonUI structures.
#[repr(C, align(2))]
pub struct WindowsSecret<const N: usize> {
    buf: [u8; N],
    len: usize,
    #[cfg(windows)]
    sealed: bool,
    #[cfg(windows)]
    locked: bool,
}

impl<const N: usize> WindowsSecret<N> {
    /// All-zero buffer; pages locked on Windows.
    pub fn new() -> Self {
        let mut s = Self {
            buf: [0u8; N],
            len: 0,
            #[cfg(windows)]
            sealed: false,
            #[cfg(windows)]
            locked: false,
        };
        #[cfg(windows)]
        s.lock_pages();
        #[cfg(not(windows))]
        let _ = &mut s;
        s
    }

    /// Writes `s` as UTF-16LE into the buffer. Clears the previous contents
    /// first; on capacity overflow the buffer stays fully zeroed.
    pub fn write_utf16le(&mut self, s: &str) -> Result<(), CapacityError> {
        self.clear();
        // s.len() is the UTF-8 byte length, not the UTF-16 unit count.
        let needed = s.encode_utf16().count().checked_mul(2).ok_or(CapacityError {
            capacity_bytes: N,
            needed_bytes: usize::MAX,
        })?;
        if needed > N {
            return Err(CapacityError { capacity_bytes: N, needed_bytes: needed });
        }
        #[cfg(windows)]
        self.unseal();
        for (i, unit) in s.encode_utf16().enumerate() {
            self.buf[i * 2] = (unit & 0xff) as u8;
            self.buf[i * 2 + 1] = (unit >> 8) as u8;
        }
        self.len = needed;
        Ok(())
    }

    pub fn len_bytes(&self) -> usize {
        self.len
    }

    pub fn len_units(&self) -> usize {
        self.len / 2
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Read-only UTF-16 view for FFI (LogonUI/KERB structures). On Windows
    /// the caller must have unsealed the buffer first.
    pub fn as_u16_ptr(&self) -> *const u16 {
        #[cfg(windows)]
        debug_assert!(!self.sealed, "read from sealed WindowsSecret");
        self.buf.as_ptr().cast()
    }

    /// Mutable UTF-16 view for FFI callers that populate the buffer in
    /// place (Phase 2 pipe read).
    pub fn as_u16_mut_ptr(&mut self) -> *mut u16 {
        #[cfg(windows)]
        debug_assert!(!self.sealed, "write to sealed WindowsSecret");
        self.buf.as_mut_ptr().cast()
    }

    /// Zero the whole buffer (password residue erased). Never fails.
    /// Volatile writes defeat dead-store elimination: the compiler must not
    /// drop the erasure just because the memory is freed right after.
    pub fn clear(&mut self) {
        let p = self.buf.as_mut_ptr();
        for i in 0..N {
            unsafe {
                p.add(i).write_volatile(0u8);
            }
        }
        self.len = 0;
    }

    /// Windows: flip the buffer pages to PAGE_NOACCESS so the credential
    /// cannot be read (or dumped) while idle. Failures are ignored
    /// (fail-open to keep the secret usable; erasure safety is in Drop).
    #[cfg(windows)]
    pub fn seal(&mut self) {
        if self.sealed {
            return;
        }
        let mut old = 0u32;
        let ok = unsafe {
            VirtualProtect(self.buf.as_mut_ptr().cast(), N, PAGE_NOACCESS, &mut old)
        };
        if ok != 0 {
            self.sealed = true;
        }
    }

    /// Windows: restore PAGE_READWRITE before any read/write.
    #[cfg(windows)]
    pub fn unseal(&mut self) {
        if !self.sealed {
            return;
        }
        let mut old = 0u32;
        let ok = unsafe {
            VirtualProtect(self.buf.as_mut_ptr().cast(), N, PAGE_READWRITE, &mut old)
        };
        if ok != 0 {
            self.sealed = false;
        }
    }

    #[cfg(windows)]
    pub fn is_sealed(&self) -> bool {
        self.sealed
    }

    #[cfg(windows)]
    pub fn is_locked(&self) -> bool {
        self.locked
    }

    #[cfg(windows)]
    fn lock_pages(&mut self) {
        let ok = unsafe { VirtualLock(self.buf.as_mut_ptr().cast(), N) };
        self.locked = ok != 0;
    }
}

impl<const N: usize> Default for WindowsSecret<N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const N: usize> Drop for WindowsSecret<N> {
    fn drop(&mut self) {
        #[cfg(windows)]
        {
            // Unseal BEFORE erasing: writing to a PAGE_NOACCESS buffer is an
            // access violation (real Windows and wine both fault).
            if self.sealed {
                self.unseal();
            }
        }
        // Fail-secure: erasure never depends on the Windows APIs.
        self.clear();
        #[cfg(windows)]
        {
            if self.locked {
                unsafe {
                    let _ = VirtualUnlock(self.buf.as_mut_ptr().cast(), N);
                }
            }
        }
    }
}

#[cfg(windows)]
#[allow(dead_code)] // only used by the (Windows-host) seal test
fn page_protection(addr: *const u8) -> u32 {
    unsafe {
        let mut mbi = MEMORY_BASIC_INFORMATION {
            BaseAddress: core::ptr::null_mut(),
            AllocationBase: core::ptr::null_mut(),
            AllocationProtect: 0,
            PartitionId: 0,
            RegionSize: 0,
            State: 0,
            Protect: 0,
            Type: 0,
        };
        let n = VirtualQuery(addr.cast(), &mut mbi, core::mem::size_of::<MEMORY_BASIC_INFORMATION>());
        if n == 0 {
            0
        } else {
            mbi.Protect
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utf16le_roundtrip() {
        let mut s = WindowsSecret::<128>::new();
        s.write_utf16le("Smile2Unlock").unwrap();
        let units: Vec<u16> = unsafe {
            core::slice::from_raw_parts(s.as_u16_ptr(), s.len_units()).to_vec()
        };
        assert_eq!(units, "Smile2Unlock".encode_utf16().collect::<Vec<_>>());
        assert_eq!(s.len_bytes(), "Smile2Unlock".len() * 2);
    }

    #[test]
    fn cjk_roundtrip() {
        let mut s = WindowsSecret::<256>::new();
        s.write_utf16le("使用面部识别登录").unwrap();
        let units: Vec<u16> = unsafe {
            core::slice::from_raw_parts(s.as_u16_ptr(), s.len_units()).to_vec()
        };
        assert_eq!(units, "使用面部识别登录".encode_utf16().collect::<Vec<_>>());
    }

    #[test]
    fn capacity_overflow_zeroes_buffer() {
        let mut s = WindowsSecret::<16>::new();
        let err = s.write_utf16le("0123456789").unwrap_err(); // needs 20 bytes
        assert!(err.needed_bytes > err.capacity_bytes);
        assert!(s.is_empty());
        let raw: &[u8] = unsafe { core::slice::from_raw_parts(s.buf.as_ptr(), 16) };
        assert!(raw.iter().all(|&b| b == 0));
    }

    #[test]
    fn overwrite_clears_previous() {
        let mut s = WindowsSecret::<64>::new();
        s.write_utf16le("old-secret").unwrap();
        s.write_utf16le("new").unwrap();
        let units: Vec<u16> = unsafe {
            core::slice::from_raw_parts(s.as_u16_ptr(), s.len_units()).to_vec()
        };
        assert_eq!(units, vec![0x006e, 0x0065, 0x0077]);
        let raw: &[u8] = unsafe { core::slice::from_raw_parts(s.buf.as_ptr(), 64) };
        // tail after "new" must still be zeroed (stale "old-secret" gone)
        assert!(raw[6..].iter().all(|&b| b == 0));
    }

    #[test]
    fn drop_zeroizes_memory() {
        // drop_in_place runs the Drop glue (which zeroizes) but keeps the
        // allocation alive, so the allocator cannot reuse the block while we
        // verify the bytes (free-then-reuse would write tcache metadata over
        // the memory and produce a false failure).
        let leaked = Box::into_raw(Box::new(WindowsSecret::<64>::new()));
        unsafe {
            (*leaked).write_utf16le("hunter2").unwrap();
        }
        let ptr = unsafe { (*leaked).buf.as_ptr() }; // buf lives at offset 0
        unsafe {
            core::ptr::drop_in_place(leaked);
        }
        let raw: &[u8] = unsafe { core::slice::from_raw_parts(ptr, 64) };
        assert!(raw.iter().all(|&b| b == 0), "memory not zeroized after drop");
    }

    #[test]
    fn u16_view_is_aligned() {
        let s = WindowsSecret::<32>::new();
        assert_eq!(s.as_u16_ptr() as usize % 2, 0);
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "wine heap faults on PAGE_NOACCESS pages; run on real Windows VM (scripts/seal_smoke.c covers wine)"]
    fn seal_changes_page_protection() {
        // Must be heap-allocated: sealing a stack-allocated buffer would make
        // the executing stack page PAGE_NOACCESS and fault on the next fetch
        // (real Windows AND wine both crash).
        let mut s = Box::new(WindowsSecret::<4096>::new());
        assert_eq!(page_protection(s.buf.as_ptr()) & 0xff, PAGE_READWRITE);
        s.seal();
        assert!(s.is_sealed());
        let prot = page_protection(s.buf.as_ptr()) & 0xff;
        // PAGE_NOACCESS may be reported alone or composed (e.g. +GUARD).
        assert_eq!(prot & 0xff, PAGE_NOACCESS);
        s.unseal();
        assert!(!s.is_sealed());
        assert_eq!(page_protection(s.buf.as_ptr()) & 0xff, PAGE_READWRITE);
    }
}
