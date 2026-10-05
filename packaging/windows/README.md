# Windows Packaging

Build and verify the Windows zip from existing MinGW release artifacts:

```bash
packaging/windows/package.sh
```

The Windows product build stages SeetaFace DLLs from the SDK package it
actually links, including TenniS CPU variants. Packaging prefers these files.
When packaging an older build without staged runtimes, `SEETAFACE_PACKAGE_ROOT`
can select one installed SDK package; only its installed `bin/x64` and
`lib/x64` DLLs are searched. Source/build-tree copies are excluded, and
multiple eligible cached versions still require an explicit selection.

CI installs Rust 1.99.0, declared in `packaging/slint/prebuilt-mingw.json`,
with `setup-rust.sh`, then seeds the MinGW Slint SDK with
`seed-slint-mingw.sh` before configuration. The scripts check the Rust compiler
commit, pinned archive digest and exact Xmake package directory, library,
headers and host compiler. A digest marker invalidates packages from older
prebuilt archives even when the Xmake package hash is unchanged.
The SDK archives come from `Smile2Unlock/local-repo` Releases. Xmake fetches
the SeetaFace MinGW SDK from that repository and verifies its SHA-256; use
`--seetaface_prebuilt=n` to build the same pinned revision from source.
Slint and the core Rust staticlib must use the
same Rust compiler; mixing releases can cause duplicate standard-library
symbols. Update the prebuilt release, digest, package identity and Rust
toolchain together. For local builds, select its `rust_toolchain` with
`RUSTUP_TOOLCHAIN` before running Xmake.

Release packages are fail-closed: provide a PEM code-signing certificate and
private key with `--sign-certificate` / `--sign-key` (or the matching
`WINDOWS_SIGN_CERTIFICATE` / `WINDOWS_SIGN_KEY` variables). Every PE payload
receives an Authenticode signature and `release-info.json` receives a detached
CMS signature. `--unsigned-development` exists only for inspecting a staged
tree; Windows deployment intentionally rejects that output.

Two trust models are supported:

- **Public CA.** Leave `SMILE2UNLOCK_PINNED_SIGNER_SHA256` unset. The deploy
  helper and GUI require a certificate chain that the Windows trust store
  accepts, and `WINDOWS_VERIFY_CA_FILE` is optional.
- **Internal self-signed.** Generate a certificate with
  `packaging/windows/generate-signing-cert.sh`, then configure/build with
  `SMILE2UNLOCK_PINNED_SIGNER_SHA256` set to its lowercase SHA-256 fingerprint
  and sign with the same certificate. The helper accepts exactly that signer
  without a public CA; file integrity still comes from the signed
  `release-info.json` manifest, which pins the SHA-256 of every installed file.
  Set `WINDOWS_VERIFY_CA_FILE` to the certificate so `verify-package.sh`
  validates the Authenticode signatures against it. Timestamping is optional
  for a self-signed certificate.

```bash
packaging/windows/generate-signing-cert.sh --output-dir .signing
export SMILE2UNLOCK_PINNED_SIGNER_SHA256=<printed fingerprint>
xmake f -y -p mingw -a x86_64 -m release --with_slint=y --with_seetaface=y
xmake build -v
WINDOWS_SIGN_CERTIFICATE=.signing/windows-signing.pem \
WINDOWS_SIGN_KEY=.signing/windows-signing.key \
WINDOWS_VERIFY_CA_FILE=.signing/windows-signing.pem \
  packaging/windows/package.sh
```

Signing material must be stored outside the checkout. GitHub automation reads
the base64-encoded certificate and private key from the protected
`release-signing` environment, materializes them only under `RUNNER_TEMP`, and
removes them when packaging finishes. Never commit or attach a private key to a
GitHub Release. Release assets contain only public packages and checksums.

The archive contains a stable `Smile2Unlock/` directory with sibling `bin/`
and `assets/` directories. `bin/Smile2UnlockCredentialProvider.dll` is the only CP DLL
name accepted by the verifier; suffixed copies are rejected.

`bin/Smile2UnlockStatus.exe` is the signed, optional-at-runtime recognition
status display launched by the credential provider on the Windows secure
desktop. It is a required package payload and is verified/staged with the
other executables. Its failure cannot authorize or block authentication. See
[status display design and acceptance](../../docs/windows_hello_status.md).

After extraction, run `bin/Smile2Unlock.exe` and use its deployment action. The UAC
helper installs security components under:

```text
C:\Program Files\Smile2Unlock\bin
```

The service ImagePath becomes
`C:\Program Files\Smile2Unlock\bin\Smile2UnlockAuthService.exe`, and the CP
`InprocServer32` value becomes
`C:\Program Files\Smile2Unlock\bin\Smile2UnlockCredentialProvider.dll`. The extraction
directory can then be removed after the application is no longer running.

Use `--stage-only` to inspect `build/package-stage/windows/Smile2Unlock`
without creating an archive. Packaging does not modify tracked repository
files; release metadata is embedded in the archive as `release-info.json`.

## Installer options

The NSIS installer verifies the signed package and deploys the required
authentication components. Its Shortcuts page offers independent desktop
(off by default) and Start Menu (on by default) choices for all users. Upgrades
apply the current choices and remove only this application's old shortcuts.
The successful completion page offers **Open Smile2Unlock**, checked by default;
unchecking it finishes without opening the application. Silent installs do not
show this page or launch the GUI.

The installer extracts and verifies a separate package before deployment. The
helper stops the existing authentication service, installs the executables,
runtime DLLs (including the TenniS CPU backends), models and translations, then
starts the service. After deployment, NSIS copies only the remaining package
metadata and license files; it never re-extracts `bin/` or `assets/` over the
running payload.

## Unicode TEMP regression

The helper and GUI use UTF-16 paths for `su_deploy_result.json`. The helper's
exit code describes the requested operation: failure to write diagnostic JSON
is logged separately and cannot turn successful verification/deployment into
exit code 1. The GUI still requires a valid result file to report success.

Build and run the focused regression under Wine (or an elevated Windows test
process, because the helper requires administrator privileges):

```bash
xmake build su_windows_deploy_helper_result_test
xmake test su_windows_deploy_helper_result_test/default -v
```

It launches the real helper with ASCII and Chinese TEMP/TMP directories,
checks its success/error JSON, and blocks result-file creation to check that
operation exit codes are preserved. The manual package CI job runs the freshly
signed package's helper with `--verify`; the focused Unicode regression remains
available for local runs with `--signed-package`.

For existing 2.3.0 installers affected by this bug, redirect TEMP and TMP to an
existing, writable ASCII-only directory in the shell launching the installer.
Check that the elevated installer actually uses that directory. This is a
temporary installation workaround; the old GUI still needs the source fix.

For a corrected release, rebuild both the GUI and helper with the release
signer pin, then run the signing/packaging command above to regenerate payload
signatures, `release-info.json`, `release-info.p7s`, ZIP and setup.exe. Replacing
only the helper in an existing signed package invalidates its manifest hash.
