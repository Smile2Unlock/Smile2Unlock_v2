# Linux Desktop PAM Integration Plan

## Status

Implementation in progress, updated 2026-07-25. Read-only detection, managed
PAM transformations, rollback, GUI controls and packaging are implemented.
This work did not modify the development machine's PAM configuration or install
desktop components. Live login, lock-screen, wallet and SELinux validation must
still run on disposable VMs or explicitly designated test systems with a
working password fallback.

## Goals

- Support face authentication in KScreenLocker, Plasma Login Manager and GDM.
- Keep the distribution's existing password, account and session stacks intact.
- Treat login-manager authentication and screen-lock authentication as separate
  capabilities where the desktop uses separate PAM services.
- Allow KWallet or GNOME Keyring to receive a PAM authentication token after a
  face login when a safe token source is available.
- Provide deterministic inspection, installation, validation and rollback for
  Arch, Fedora, Debian/Ubuntu and openSUSE PAM layouts.
- Preserve the current daemon boundary: the PAM module remains a thin client and
  `su_authd` owns camera access, profile access and recognition policy.

## Non-goals

- Do not add users to `nopasswdlogin`; enrollment and a successful face match
  are the authorization condition.
- Do not modify `system-auth`, `password-auth`, `common-auth` or other global PAM
  stacks that would affect `sudo`, SSH, TTY login or unrelated services.
- Do not modify autologin, greeter-bootstrap, fingerprint or smart-card PAM
  services.
- Do not disable SELinux, PAM failure policy or the system password fallback.
- Do not store a Linux account password in the first implementation.
- Do not patch KScreenLocker, Plasma Login Manager or GNOME Shell merely to add
  branded progress UI. PAM-level compatibility comes first.

## Confirmed Entry Points

| Environment | PAM service | Scope | Must not be confused with |
| --- | --- | --- | --- |
| KScreenLocker | `kde` by default | Plasma session unlock | `kde-fingerprint`, `kde-smartcard` |
| Plasma Login Manager | `plasmalogin` | Interactive system login | `plasmalogin-autologin`, `plasmalogin-greeter` |
| GNOME/GDM | `gdm-password` | System login and session reauthentication | `gdm-autologin`, `gdm-fingerprint`, `gdm-smartcard`, `gdm-launch-environment` |
| SDDM, compatibility phase | `sddm` | Interactive system login | `sddm-autologin`, `sddm-greeter` |
| DMS, already implemented | `dankshell-smile2unlock` | DMS session unlock | DMS-generated per-user PAM files |

KScreenLocker can be compiled with a service other than `kde`. Detection must
inspect the installed service and package metadata where possible, and the GUI
must allow an explicit supported override instead of assuming the upstream
default on every distribution.

## PAM Composition

### Lock-screen path

KScreenLocker only needs an authentication decision. Its wallet is normally
already open in the existing desktop session. A dedicated substack should try
Smile2Unlock first and continue into the original `kde` password stack for any
rejection, timeout, busy result or unavailable service.

The implementation must preserve the original service's control semantics and
must never replace it with a generic `login` template. A manually closed wallet
is outside lock-screen authentication and may still request its own password.

### Login path

A top-level line such as:

```pam
auth sufficient pam_smile2unlock.so
```

is sufficient for login but is incomplete for wallet integration. On success,
`sufficient` terminates the complete auth stack, so later `pam_kwallet5` or
`pam_gnome_keyring` modules do not receive an authentication token.

The target design therefore keeps wallet modules in the parent service and
moves only the choice between face and the original password stack into a
dedicated `substack`:

```pam
# Parent service, conceptual layout only.
auth substack smile2unlock-login
-auth optional pam_gnome_keyring.so
-auth optional pam_kwallet5.so
```

The face branch may optionally obtain the boot-time LUKS passphrase through
`pam_systemd_loadkey`. The password branch must retain the password entered by
the user and must not overwrite it with a LUKS token.

The candidate branch structure below must be proven with isolated PAM tests
before it becomes an installed template:

```pam
# Dedicated substack, conceptual layout only.
auth [success=2 default=ignore] pam_smile2unlock.so
auth substack <distribution-password-auth-stack>
auth [success=2 default=ignore] pam_permit.so
-auth optional pam_systemd_loadkey.so
auth required pam_permit.so
```

The intended paths are:

| Result | Password stack | LUKS token load | Parent wallet modules |
| --- | --- | --- | --- |
| Face accepted | Skipped | Attempted | Always continue |
| Face rejected/unavailable | Required | Skipped | Continue after password success |
| Password rejected | Failed | Skipped | Overall authentication fails |

Tests must verify Linux-PAM jump and substack semantics directly. Production
installation must not rely only on a visual review of the configuration.

## Wallet And Keyring Policy

### Preferred path: systemd and LUKS

When systemd 255 or newer and a suitable boot-time key are available,
`pam_systemd_loadkey` can set `PAM_AUTHTOK` from the root kernel keyring. This
allows `pam_kwallet5` or `pam_gnome_keyring` to unlock a wallet whose password
matches the LUKS passphrase without Smile2Unlock storing the account password.

Requirements:

- The display-manager service can inherit the relevant kernel keyring, usually
  through a reviewed `KeyringMode=inherit` systemd override.
- The wallet or login keyring password matches the retained LUKS passphrase.
- The boot flow actually places an appropriate passphrase in the keyring.
- The key is loaded only after face success, never before the authentication
  decision where it could accidentally satisfy the Unix password stack.

If any requirement is missing, face login still succeeds but the wallet may
prompt after login. There is no plaintext or empty-password fallback.

### Unsupported automatic-unlock cases

- TPM2-only disk unlock with no usable passphrase in the kernel keyring.
- Wallet password differs from the available LUKS passphrase.
- Older systemd without `pam_systemd_loadkey`.
- Multiple users whose wallets cannot safely share the boot-time passphrase.
- Encrypted or systemd-homed user data unavailable before account password
  authentication.

An optional, separately reviewed Linux credential escrow can be planned later
using the existing TPM2/host-key storage foundation. It must not be smuggled
into this phase because it expands password-change, memory-lifetime and root IPC
security requirements.

## Distribution-safe Configuration

The PAM integrator must understand both `/etc/pam.d` administrator overrides
and vendor files under `/usr/lib/pam.d`. It must:

1. Resolve the effective service file without following symlinks into unsafe
   locations.
2. Parse PAM fields and control expressions structurally; comments and include
   directives are not handled by substring replacement.
3. Select an exact, versioned transformation for a recognized service layout.
4. Produce a dry-run plan containing source, destination, inserted substack and
   preserved password/account/session entries.
5. Refuse unknown, ambiguous or already externally modified layouts.
6. Write an administrator override atomically and retain a root-only backup.
7. Record enough metadata to remove only Smile2Unlock-owned changes.

Separate fixtures are required for upstream and distribution variants of
Arch, Fedora, Debian/Ubuntu and openSUSE. Fedora placement must preserve
SELinux-related PAM entries and `postlogin`; Debian placement must preserve
`pam_nologin`, root policy and `common-*`; no generic template may flatten these
differences.

## Runtime Integration

### Camera coordination

The current GUI listens only for the logind `Session.Lock` signal. KDE and
GNOME can lock internally and then update the session's `LockedHint` without
causing that signal to be delivered to every client.

Extend the session monitor to release the GUI camera on:

- `org.freedesktop.login1.Session.Lock`;
- `PropertiesChanged` with `LockedHint=true`;
- `PrepareForSleep(true)`.

The callback must remain idempotent. The daemon's bounded camera-open retry is
retained for unavoidable races.

### Status model

Replace the single `pam_configured` flag with explicit capabilities:

- authentication service installed and reachable;
- interactive login configured;
- session lock configured;
- password fallback verified structurally;
- optional wallet-token path available;
- configuration managed by Smile2Unlock or external;
- last isolated acceptance result.

Plasma normally requires both `plasmalogin` and `kde`. GDM's `gdm-password`
can satisfy both login and lock capabilities. A configured unrelated PAM
service must not make onboarding appear complete.

## SELinux

On Fedora with SELinux enforcing, validate display-manager and screen-locker
connections to `/run/smile2unlock/control.sock` using audit logs. If policy is
required, ship a minimal versioned policy package for the actual daemon and PAM
domains. Never recommend permissive mode or a broad allow rule generated
without review.

## Implementation Phases

### Phase 0: Freeze fixtures and behavior

- [x] Add isolated representative fixtures for Arch Plasma, KScreenLocker,
  Fedora GDM, Debian/Ubuntu GDM and SDDM without modifying host PAM files.
- [ ] Capture versioned upstream and openSUSE variants rather than relying only
  on the current inline representative fixtures.
- [x] Define the service-capability and transformation data models.
- [ ] Build isolated Linux-PAM tests for face success, password fallback,
  unavailable daemon, jump semantics and downstream wallet module execution.
  The module/socket result mapping is covered; full parent/substack and wallet
  execution semantics remain open.

### Phase 1: Read-only detection

- [x] Resolve effective PAM service files from `/etc` and vendor directories.
- [x] Detect KScreenLocker, Plasma Login Manager, GDM and SDDM separately.
- [x] Report recognized, unsupported, externally managed and partially
  configured states without modifying files.
- [x] Split GUI diagnostics into login, lock and wallet capabilities.

### Phase 2: KScreenLocker

- [x] Generate and validate a `kde` lock-screen substack.
- [x] Preserve password unlock for rejected, busy, timed-out and unavailable
  face requests.
- [x] Add `LockedHint` and suspend camera-release handling.
- [ ] Validate lock, unlock, suspend/resume and camera contention in Plasma VMs.

### Phase 3: Plasma Login Manager

- [x] Transform only the interactive `plasmalogin` service.
- [x] Preserve distribution account, password, session, SELinux and keyring
  modules.
- [ ] Validate user selection, face success, password fallback, user switching,
  logout and cold boot in an isolated VM.

### Phase 4: GNOME/GDM

- [x] Transform only `gdm-password` and retain GNOME Keyring/postlogin entries.
- [ ] Validate both greeter login and GNOME Shell reauthentication.
- [ ] Confirm lock-screen failure returns to the password prompt without
  restarting the session.
- [ ] Validate SELinux enforcing on Fedora rather than disabling policy.

### Phase 5: Optional wallet token

- [ ] Detect systemd version, `pam_systemd_loadkey`, inherited keyring support
  and an available boot key without exposing its contents.
- [x] Implement the generated face-only `pam_systemd_loadkey` substack while
  preserving the original password/keyring modules in the parent service. The
  privileged helper still refuses to enable it until eligibility is proven.
- [ ] Test matching, mismatched and absent LUKS tokens for KWallet and GNOME
  Keyring.
- [x] Document the TPM-only, mismatched-password, old-systemd and multi-user
  limitations; these cases retain normal login and allow the wallet to prompt.

### Phase 6: SDDM and packaging

- [x] Add SDDM interactive-login compatibility without touching autologin.
- [ ] Package the remaining versioned parser fixtures and optional SELinux
  policy. Managed PAM substacks and rollback journal metadata are already
  produced by the deployment engine.
- [x] Keep package installation side-effect free; desktop authentication is
  enabled only through an explicit post-install action.

## Validation Matrix

- Arch: Plasma Login + KScreenLocker, password and LUKS-wallet paths.
- Fedora: Plasma Login and GDM under SELinux enforcing.
- Debian/Ubuntu: GDM and the distribution `common-*` stack.
- openSUSE: Plasma and `common-*`/`postlogin-*` composition.
- Service stopped, socket missing, model missing, no profile and camera busy.
- Face accepted, face rejected, liveness timeout and cancellation.
- Password fallback after every non-success face result.
- Existing password login still runs account and session policy.
- KWallet/GNOME Keyring with matching, missing and mismatched token.
- Install twice, upgrade, external edit after install, rollback and uninstall.

Every live display-manager test requires a working password path, an available
TTY or recovery console, and a snapshot or documented rollback. Automated tests
must use isolated PAM configuration directories and filesystem staging roots.

## Completion Criteria

- A package-installed user can explicitly enable the detected login and lock
  targets without editing PAM by hand.
- Face success and password fallback work in Plasma Login, KScreenLocker and
  GDM on their supported distributions.
- No autologin, SSH, `sudo`, TTY or unrelated PAM service is modified.
- GUI diagnostics distinguish login, lock and wallet-token readiness accurately.
- All managed changes are inspectable, idempotent and fully reversible.
- Wallet auto-unlock is enabled only when its token source is proven available;
  otherwise login remains functional and the wallet prompts normally.
