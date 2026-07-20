# Windows Credential Storage

Smile2Unlock stores the current user's Windows logon password in:

```text
%ProgramData%\Smile2Unlock\Users\<SID>\logon-secret.s2u
```

The LocalSystem `Smile2UnlockAuthService` is the only process that opens the
master key and encrypted secret files. The desktop app can only replace or
clear the secret for its own process-token SID. The Credential Provider can
request a password only after face verification and only for one request id
and LogonUI session.

On a machine with a working Microsoft Platform Crypto Provider, the service
uses a non-exportable TPM RSA key to wrap the storage master key. It uses
machine DPAPI only when that provider is explicitly unavailable. A failure of
an existing TPM provider does not trigger a runtime downgrade.

Build and stage the service and Credential Provider from PowerShell:

```powershell
packaging\windows\build-storage.ps1
```

The script writes both artifacts to `installer-files`, where `setup.iss`
includes them. The installer registers the service as an automatic LocalSystem
service and starts it after installation.

## Upgrade Behavior

The legacy SQLite `encrypted_password` values and adjacent AES key are retired
when the old Windows backend first opens its database. They are not silently
decrypted or migrated. Each user must sign in normally, open Smile2Unlock, and
enter the current Windows account password again. A Windows Hello PIN is not a
valid replacement.

The first supported account types are local Windows accounts and Microsoft
accounts. Domain and Entra accounts remain disabled until their password
rotation and offline-logon behavior have separate acceptance coverage.
