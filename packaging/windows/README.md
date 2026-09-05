# Windows Packaging

Build and verify the Windows zip from existing MinGW release artifacts:

```bash
packaging/windows/package.sh
```

Release packages are fail-closed: provide a trusted PEM code-signing
certificate and private key with `--sign-certificate` / `--sign-key` (or the
matching `WINDOWS_SIGN_CERTIFICATE` / `WINDOWS_SIGN_KEY` variables). Every PE
payload receives an Authenticode signature and `release-info.json` receives a
detached CMS signature. `--unsigned-development` exists only for inspecting a
staged tree; Windows deployment intentionally rejects that output.

Formal signing material must be stored outside the checkout. GitHub automation
reads the base64-encoded certificate chain and private key from the protected
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
