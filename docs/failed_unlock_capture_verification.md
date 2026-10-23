# #117 part 3 verification

Verified in this worktree on 2026-10-22. No push, PR, host PAM changes, service installation, or production capture-store writes were performed. The control-socket integration fixture uses isolated user/mount namespaces and a private `/var/lib` tmpfs.

## Results

| Check | Exact result |
| --- | --- |
| Native Rust core tests | 55 passed, 0 failed, 0 ignored |
| Native Rust core Clippy / fmt | exit 0 / exit 0 |
| Linux Debug build, SeetaFace and Slint enabled | exit 0; `su_authd`, `su_app`, JPEG/UI fixtures and seven focused test targets built |
| Linux focused tests | 7 passed, 0 failed; includes `su_pam_integration_test/default` |
| Real Linux daemon capture socket | exit 0; own configure/list/delete-all accepted; foreign uid (including root requesting another account), bad versions, unknown operations/fields, path traversal and invalid settings rejected |
| MinGW Release build, SeetaFace and Slint enabled | exit 0; `su_auth_service`, `su_recognition_agent`, `su_app` and five focused test targets built |
| Wine C++ focused tests | 5 passed, 0 failed |
| Wine Rust core tests | 53 passed, 0 failed, 0 ignored; two Unix-only tests excluded by cfg |
| Wine Credential Provider tests | main suite: 128 passed, 0 failed, 1 ignored; its isolated `log::platform::tests::child_writer` subprocess: 1 passed, 128 filtered |
| Credential Provider Clippy / fmt | exit 0 / exit 0 |
| Capture viewer renders | en + zh-CN, populated + empty: four successful runs; PNGs visually inspected |
| Whitespace | `git diff --check`: exit 0 |

Builds retain existing Slint anonymous-struct pedantic warnings and MinGW camera warnings; Xmake reports the optional Ascend SDK absent. Wine emits staging/Mesa diagnostics. These did not cause failures. Two small pre-existing Rust password-code Clippy warnings were fixed in a separate commit so `-D warnings` passes with Rust 1.99.

## Exact commands

From the worktree root, unless a working directory is explicitly shown:

```sh
cargo test --locked --manifest-path src/core-rs/Cargo.toml
cargo clippy --locked --manifest-path src/core-rs/Cargo.toml --all-targets -- -D warnings
cargo fmt --check --manifest-path src/core-rs/Cargo.toml

xmake f -p linux -a x86_64 -m debug --with_slint=y --with_seetaface=y -y
xmake build -r su_authd su_app su_capture_image_test su_capture_ui_test su_pam_integration_test su_control_socket_smoke_test su_control_socket_deadline_test su_multi_user_auth_test su_key_provider_test su_capture_pipeline_test
xmake test -v su_capture_image_test/default su_pam_integration_test/default su_control_socket_smoke_test/default su_control_socket_deadline_test/default su_multi_user_auth_test/default su_key_provider_test/default su_capture_pipeline_test/default
python3 tests/capture/control_socket.py build/linux/x86_64/debug/su_authd
# After isolating capture key access, rebuild and rerun the real daemon fixture:
xmake build su_authd
python3 tests/capture/control_socket.py build/linux/x86_64/debug/su_authd

xmake f -p mingw -a x86_64 -m release --with_slint=y --with_seetaface=y -y
xmake build -r su_auth_service su_recognition_agent su_app su_windows_capture_protocol_test su_windows_recognition_result_test su_windows_pipe_response_test su_windows_request_logging_test su_capture_image_test
xmake test -v su_windows_capture_protocol_test/default su_windows_recognition_result_test/default su_windows_pipe_response_test/default su_windows_request_logging_test/default su_capture_image_test/default

env CARGO_TARGET_X86_64_PC_WINDOWS_GNU_RUNNER=wine cargo test --locked --target x86_64-pc-windows-gnu --manifest-path src/core-rs/Cargo.toml
```

With working directory `src/platform/windows/credential_provider_rs` (so Cargo discovers the checked-in Wine runner):

```sh
cargo test --locked --target x86_64-pc-windows-gnu
cargo clippy --locked --all-targets --target x86_64-pc-windows-gnu -- -D warnings
cargo fmt --check
```

UI render commands after the Linux build:

```sh
xvfb-run -a env SLINT_BACKEND=winit-software build/linux/x86_64/debug/su_capture_ui_test assets/i18n/en.json /tmp/s2u-work/capture-en-populated.ppm populated
xvfb-run -a env SLINT_BACKEND=winit-software build/linux/x86_64/debug/su_capture_ui_test assets/i18n/zh-CN.json /tmp/s2u-work/capture-zh-populated.ppm populated
xvfb-run -a env SLINT_BACKEND=winit-software build/linux/x86_64/debug/su_capture_ui_test assets/i18n/en.json /tmp/s2u-work/capture-en-empty.ppm empty
xvfb-run -a env SLINT_BACKEND=winit-software build/linux/x86_64/debug/su_capture_ui_test assets/i18n/zh-CN.json /tmp/s2u-work/capture-zh-empty.ppm empty
magick /tmp/s2u-work/capture-en-populated.ppm /tmp/s2u-work/capture-en-populated.png
magick /tmp/s2u-work/capture-zh-populated.ppm /tmp/s2u-work/capture-zh-populated.png
magick /tmp/s2u-work/capture-en-empty.ppm /tmp/s2u-work/capture-en-empty.png
magick /tmp/s2u-work/capture-zh-empty.ppm /tmp/s2u-work/capture-zh-empty.png
git diff --check
```

The initially uninitialized `local-repo` submodule was initialized at the existing pinned commit `7e4b4091cafabf1efacefb42665c9a1f0b477653`; no submodule revision changed. Build and test logs are `/tmp/s2u-work/capture-*.log`. The worktree is left configured for Linux Debug with Slint and SeetaFace enabled; MinGW Release artifacts remain in `build/mingw/x86_64/release/`.

## Acceptance limits

These checks exercise pure policy/retention/metadata, encrypted account isolation, tampering, opt-out during an attempt, deletion, bounded JPEG encoding, agent protocol validation, actual Linux peer credentials, and UI layout. Capture I/O uses an independently sealed Linux key handle loaded by the existing provider, so it does not hold the authentication handle access mutex. The render fixture uses a synthetic green image, not a real person's photograph.

Real camera failure/mismatch/liveness capture on a native Windows secure desktop, TPM/DPAPI hardware acceptance, and native Linux greeter/device acceptance were not run. Wine and isolated Linux tests do not establish those device-level results. Automatic retention is implemented by the service's stop-aware one-minute maintenance sweep, with expiry/count/byte decisions covered by deterministic core tests; no multi-day wall-clock wait was performed.
