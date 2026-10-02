# Windows 26H2 deployment and status-display follow-up

This records the development snapshot after PR #103, compared with remote
`main` at `f13c42a`. The original Hello-style status display is already on main.

## Changes in this snapshot

- Credential-provider registration requests subkey creation rights and verifies
  that its enrollment key can be reopened after registration.
- Deployment compares installed files with the signed manifest hash before
  replacing identical bytes, including an already loaded provider and a helper
  launched from its own installation directory.
- NSIS verifies a separate staged package before deployment, then publishes the
  ZIP layout at the installation root so the GUI can find the signed manifest.
- The status display moves from 8% to 1.5% of screen height. The runtime publishes
  an initial waiting hint while still hiding it after a sign-in method change.
- An experimental UIAccess child and `CreateWindowInBand` path, with ordinary
  window fallback, attempts to show the hint on the clock curtain. The display
  follows input desktop changes and checks session lock state on Default.

## Verification and limitations

The MinGW product build and the final status-host build passed. The changed
runtime suite passed 16 native Windows tests; the status snapshot suite passed
3 tests. These tests do not verify desktop composition or UIAccess behavior.

The signed 2.3.1 development installer was installed over SSH on
`win11-sleep-test`, Windows 11 26H2 build 26300.9457. Installer exit code was 0,
installed manifest verification passed, enrollment was managed, and the auth
service was running. Installed CP, status-host and helper hashes matched the
signed package. No release tag was created.

**The user reports that the clock curtain still has no status hint with this
build. This is an unresolved experiment, not a working clock-screen fix.**
The foreground/desktop observations are diagnostic evidence, not proof that
UIAccess can overlay LockApp. The private band API and desktop switching need
review before this snapshot is considered merge-ready. The previous Windows 10
acceptance in `windows_hello_status.md` applies to PR #103, not this experiment.

Sleep/hibernate/lid-open rearming is not implemented by these changes. On
2026-10-03, `powercfg /a` on this VM reports hibernation and fast startup available;
S3 is blocked by graphics, and S0 low-power idle is unsupported. No battery or
lid device was found. Historical Kernel-Power 42/107 events alone do not prove
successful S3 sleep, hibernation, or physical lid-open behavior.

Detailed local build, installation and diagnostic artifacts remain in the
ignored `build/windows-hello-status-20261002/` directory. Credentials, signing
keys and diagnostic binaries are not part of this snapshot.
