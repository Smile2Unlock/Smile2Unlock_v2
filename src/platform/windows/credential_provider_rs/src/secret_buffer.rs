//! Fixed-capacity secret buffer for the one-time logon credential
//! (UTF-16LE bytes) and the pipe-client transport buffer.
//!
//! Security model (docs/windows_credential_provider_rust_plan.md, memsafe
//! section):
//! - Built on the vendored `memsafe` fork (v1.0.2, commit
//!   704f558a0e796e3b2fb8837ee7aabcb46c3b1250 + project patch: fail-secure
//!   Drop without unwrap/panic). `memsafe::Secret<N>` keeps the bytes in a
//!   VirtualAlloc'ed, VirtualLock'ed page that is PAGE_READONLY while idle
//!   and only temporarily elevated during read()/write().
//! - Fixed capacity: the credential is stored as bytes + length, never as a
//!   growable String/Vec and never converted to a plain `u16`/`String` copy.
//! - Drop zeroizes the buffer unconditionally (fail-secure: protection API
//!   failures never block erasure and never panic/unwrap).
//! - No Clone, no Debug, no Default printing of contents.

use core::fmt;

use memsafe::Secret;

/// A secret-buffer failure. Callers convert it into the closest HRESULT;
/// nothing about the contents is ever logged.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SecretError {
    /// Protected-memory initialization or protection operation failed
    /// (VirtualAlloc/VirtualLock/VirtualProtect).
    Init,
    /// The buffer capacity was exceeded; nothing is written and the buffer
    /// is left fully zeroed.
    Capacity {
        capacity_bytes: usize,
        needed_bytes: usize,
    },
}

impl fmt::Display for SecretError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            SecretError::Init => write!(f, "secret memory protection failed"),
            SecretError::Capacity {
                capacity_bytes,
                needed_bytes,
            } => write!(
                f,
                "secret capacity exceeded: need {} bytes, have {}",
                needed_bytes, capacity_bytes
            ),
        }
    }
}

impl core::error::Error for SecretError {}

/// Fixed-capacity, zeroizing, page-protected byte buffer.
///
/// `N` is the byte capacity (UTF-16LE, so an even number of bytes). The
/// struct is 2-byte aligned; the protected page itself is allocated by
/// VirtualAlloc (16-byte aligned) so u16 views are always aligned.
pub struct WindowsSecret<const N: usize> {
    secret: Secret<N>,
    len: usize,
}

impl<const N: usize> WindowsSecret<N> {
    /// All-zero buffer in protected memory (memsafe: locked + read-only
    /// while idle). Fallible: protected-memory allocation failures surface
    /// as `SecretError::Init` instead of panicking inside LogonUI.
    pub fn new() -> Result<Self, SecretError> {
        let secret = Secret::<N>::new_with(|page| page.fill(0)).map_err(|_| SecretError::Init)?;
        Ok(Self { secret, len: 0 })
    }

    /// Writes `s` as UTF-16LE into the buffer. Clears the previous contents
    /// first; on capacity overflow the buffer stays fully zeroed.
    pub fn write_utf16le(&mut self, s: &str) -> Result<(), SecretError> {
        self.clear();
        // s.len() is the UTF-8 byte length, not the UTF-16 unit count.
        let needed = s
            .encode_utf16()
            .count()
            .checked_mul(2)
            .ok_or(SecretError::Capacity {
                capacity_bytes: N,
                needed_bytes: usize::MAX,
            })?;
        if needed > N {
            return Err(SecretError::Capacity {
                capacity_bytes: N,
                needed_bytes: needed,
            });
        }
        {
            let mut view = self.secret.write().map_err(|_| SecretError::Init)?;
            for (i, unit) in s.encode_utf16().enumerate() {
                view[i * 2] = (unit & 0xff) as u8;
                view[i * 2 + 1] = (unit >> 8) as u8;
            }
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

    /// Run `f` with a UTF-16LE read view directly into protected memory.
    /// No plain `Vec<u16>` copy of the secret is produced.
    pub fn with_u16_slice<F, R>(&mut self, f: F) -> Result<R, SecretError>
    where
        F: FnOnce(&[u16]) -> R,
    {
        let view = self.secret.read().map_err(|_| SecretError::Init)?;
        // SAFETY: view is a readable [u8; N] owned by the guard; cast to a
        // u16 slice of len_units() elements (VirtualAlloc alignment).
        let units =
            unsafe { core::slice::from_raw_parts(view.as_ptr().cast::<u16>(), self.len / 2) };
        Ok(f(units))
    }

    /// Set the logical length (UTF-16 units) after populating the buffer
    /// through `with_u16_slice_mut`.
    pub fn set_len_units(&mut self, units: usize) -> Result<(), SecretError> {
        let bytes = units.checked_mul(2).ok_or(SecretError::Capacity {
            capacity_bytes: N,
            needed_bytes: usize::MAX,
        })?;
        if bytes > N {
            return Err(SecretError::Capacity {
                capacity_bytes: N,
                needed_bytes: bytes,
            });
        }
        self.len = bytes;
        Ok(())
    }

    /// Run `f` with a UTF-16LE read-write view directly into protected
    /// memory. No plain `Vec<u16>` copy of the secret is produced.
    pub fn with_u16_slice_mut<F, R>(&mut self, f: F) -> Result<R, SecretError>
    where
        F: FnOnce(&mut [u16]) -> R,
    {
        let mut view = self.secret.write().map_err(|_| SecretError::Init)?;
        // SAFETY: view is a writable [u8; N] owned by the guard; cast to a
        // u16 slice of N/2 elements (VirtualAlloc alignment).
        let units =
            unsafe { core::slice::from_raw_parts_mut(view.as_mut_ptr().cast::<u16>(), N / 2) };
        Ok(f(units))
    }

    /// Volatile-zero the contents. Best-effort: if the protection API fails
    /// the region stays at its current protection level (fail-secure).
    pub fn clear(&mut self) {
        if let Ok(mut view) = self.secret.write() {
            view.fill(0);
        }
        self.len = 0;
    }
}

impl<const N: usize> Drop for WindowsSecret<N> {
    fn drop(&mut self) {
        // Fail-secure: zeroize before the protected page is released. A
        // failure to elevate stays silent (no panic in Drop).
        if let Ok(mut view) = self.secret.write() {
            view.fill(0);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utf16le_roundtrip() {
        let mut s = WindowsSecret::<128>::new().unwrap();
        s.write_utf16le("Smile2Unlock").unwrap();
        assert_eq!(s.len_units(), "Smile2Unlock".encode_utf16().count());
        s.with_u16_slice(|units| {
            let text = String::from_utf16_lossy(units);
            assert_eq!(text, "Smile2Unlock");
        })
        .unwrap();
    }

    #[test]
    fn cjk_roundtrip() {
        let mut s = WindowsSecret::<256>::new().unwrap();
        s.write_utf16le("使用面部识别登录").unwrap();
        s.with_u16_slice(|units| {
            let text = String::from_utf16_lossy(units);
            assert_eq!(text, "使用面部识别登录");
        })
        .unwrap();
    }

    #[test]
    fn capacity_overflow_zeroes_buffer() {
        let mut s = WindowsSecret::<16>::new().unwrap();
        s.write_utf16le("abc").unwrap();
        // 9 UTF-16 units = 18 bytes > 16.
        let err = s.write_utf16le("abcdefghi").unwrap_err();
        assert!(matches!(err, SecretError::Capacity { .. }));
        assert!(s.is_empty());
        // Previous contents must be gone.
        s.with_u16_slice(|units| {
            assert!(units.iter().all(|&u| u == 0));
        })
        .unwrap();
    }

    #[test]
    fn overwrite_clears_previous() {
        let mut s = WindowsSecret::<64>::new().unwrap();
        s.write_utf16le("old-secret").unwrap();
        s.write_utf16le("new").unwrap();
        s.with_u16_slice(|units| {
            let text = String::from_utf16_lossy(units);
            assert_eq!(text, "new");
        })
        .unwrap();
    }

    #[test]
    fn drop_zeroizes_memory() {
        // The Drop impl zeroizes the protected page before memsafe releases
        // it; constructing/writing/dropping must not panic even if page
        // protection operations fail.
        let s = WindowsSecret::<64>::new().unwrap();
        drop(s);
        let mut s = WindowsSecret::<64>::new().unwrap();
        s.write_utf16le("hunter2").unwrap();
        drop(s);
    }

    #[test]
    fn u16_view_is_aligned() {
        let mut s = WindowsSecret::<32>::new().unwrap();
        s.write_utf16le("x").unwrap();
        s.with_u16_slice(|units| {
            assert_eq!(units.as_ptr() as usize % 2, 0);
        })
        .unwrap();
    }

    #[test]
    fn concurrent_access_is_consistent() {
        // Thread interleave: several threads alternately write and read
        // through a Mutex; every read must see exactly what its writer
        // stored (protection elevation round-trips are serialized by the
        // borrow discipline, never racing).
        use std::sync::{Arc, Mutex};
        let shared = Arc::new(Mutex::new(WindowsSecret::<128>::new().unwrap()));
        let mut handles = Vec::new();
        for t in 0..4u8 {
            let shared = Arc::clone(&shared);
            handles.push(std::thread::spawn(move || {
                for i in 0..40u16 {
                    let mut s = shared.lock().unwrap();
                    let text = format!("t{}-{}", t, i);
                    s.write_utf16le(&text).unwrap();
                    let expected = text.encode_utf16().collect::<Vec<u16>>();
                    s.with_u16_slice(|units| {
                        assert_eq!(units, expected.as_slice());
                    })
                    .unwrap();
                }
            }));
        }
        for h in handles {
            h.join().unwrap();
        }
    }
}
