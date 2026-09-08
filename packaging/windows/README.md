# Windows Packaging

Build and verify the Windows zip from existing MinGW release artifacts:

```bash
packaging/windows/package.sh
```

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
and `assets/` directories. `bin/su_credential_provider.dll` is the only CP DLL
name accepted by the verifier; suffixed copies are rejected.

After extraction, run `bin/su_app.exe` and use its deployment action. The UAC
helper installs security components under:

```text
C:\Program Files\Smile2Unlock\bin
```

The service ImagePath becomes
`C:\Program Files\Smile2Unlock\bin\Smile2UnlockAuthService.exe`, and the CP
`InprocServer32` value becomes
`C:\Program Files\Smile2Unlock\bin\su_credential_provider.dll`. The extraction
directory can then be removed after the application is no longer running.

Use `--stage-only` to inspect `build/package-stage/windows/Smile2Unlock`
without creating an archive. Packaging does not modify tracked repository
files; release metadata is embedded in the archive as `release-info.json`.
