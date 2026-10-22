# Issue #117 parts 1 and 2: verification

Verified locally on 2026-10-22, including review round 1. Part 3 (failed-unlock
photos) is out of scope.
No GUI sources, host PAM configuration or login services were changed.

## Behavior

- An explicit empty-password submit restarts one capture from `Stopped` for any
  reason (`TimedOut`, `NoMatch`, `Unavailable`, or no recorded reason) in either
  trigger mode. Failed/completed Windows logons, invalid broker passwords and
  submitted grants still refuse resubmission; refresh-driven reselection cannot
  restart a stopped attempt. Service/model unavailability offers Enter / → retry;
  failed Windows logon guidance points only to the Windows password.
- Agent no-face results use Windows protocol-v2 status 8. Liveness rejection
  and profile mismatch retain authentication-failed status; service/transport
  timeout mappings are unchanged. Existing completion logs name the new status.
- The CP uses a shared English / Simplified Chinese text table, selected by the
  Windows UI primary language. GIT notifications refresh recognition, retry and
  stopped guidance without passing LogonUI interfaces across threads.
- Linux responses optionally include `authentication_reason` (`no_face` or
  `mismatch`); absent or unknown values mean unknown. Existing diagnostic
  `reason` is preserved for compatibility and never displayed verbatim by PAM.
  PAM sends `PAM_TEXT_INFO` before recognition and on failure, respects
  `PAM_SILENT`, and preserves return codes and the 8-second socket deadline.
  Language selection checks PAM `LC_MESSAGES`, PAM `LANG`, process
  `LC_MESSAGES`, then process `LANG`; `zh` selects Chinese, otherwise English.

## Commands and results

The pinned package submodule was initially absent:

```sh
git submodule update --init local-repo
```

Passed, checked out `7e4b4091cafabf1efacefb42665c9a1f0b477653`.

From `src/platform/windows/credential_provider_rs`:

```sh
cargo test --locked --target x86_64-pc-windows-gnu
cargo clippy --locked --target x86_64-pc-windows-gnu --all-targets -- -D warnings
cargo fmt --check
```

Passed: 126 tests, 0 failed, 1 existing ignored test; the isolated secret-sealing
subprocess test also passed. The ignored test depends on `CredIsProtectedW`,
which Wine does not implement. Clippy and formatting passed with exit 0.
Tests cover explicit retry after unavailability in both trigger modes, no restart
on reselection, one capture per retry, Windows-login and submitted-grant refusal,
transient wire classification, split unavailable/login-failed copy, owner-scoped
presentation, and worker notifications for no-face and mismatch.

From the repository root (MinGW release with real SeetaFace enabled):

```sh
xmake f -c -y -p mingw -a x86_64 -m release --with_slint=n --with_seetaface=y
xmake build -y su_auth_service su_recognition_agent su_credential_provider su_windows_recognition_result_test su_windows_request_logging_test
xmake test -v su_windows_recognition_result_test/default su_windows_request_logging_test/default
```

Passed: all requested binaries including the CP DLL built; 2/2 C++ tests passed
under Wine, 0 failed (review round 1: 0.010 seconds total). The release build
completed in 5.585 seconds. Earlier verification reported an existing
`-Wglobal-module` warning in untouched `src/recognizer/image/pixel_convert.cpp`
and Wine environment / EGL messages.

Linux release:

```sh
xmake clean -a
xmake f -c -y -p linux -a x86_64 -m release --with_slint=n --with_seetaface=y
xmake build -y pam_smile2unlock su_authd su_pam_integration_test su_control_socket_smoke_test su_control_socket_deadline_test
xmake test -v su_pam_integration_test/default su_control_socket_smoke_test/default su_control_socket_deadline_test/default
```

Passed: PAM module, daemon and tests built; 3/3 suites passed, 0 failed. PAM
integration took 24.143 seconds (24.145 seconds for all three suites), covering the existing timeout/password fallback
cases plus 14 guidance cases (both languages, no-face/mismatch/unknown, success,
silent flags, PAM/process locale and locale precedence) and response compatibility.
Switching from MinGW initially hit stale C++ module metadata, including after
`xmake f -c`; `xmake clean -a` followed by reconfiguration resolved it. Earlier
verification fixed an overlong private test socket path. No host PAM installation
was needed.

```sh
git diff --check
```

Passed. New files are below 800 lines; new functions are below 50 lines.

## Not verified

Native Windows LogonUI layout, secure-desktop/LSA behavior, and physical camera /
liveness acceptance were not run. Linux tests use a private PAM configuration and
a fake daemon; physical-camera daemon recognition and a production greeter were
not exercised. Local/Wine verification does not establish native acceptance.
