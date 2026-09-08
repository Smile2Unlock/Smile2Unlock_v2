# Linux GUI Deployment Plan

## Status

Implementation in progress, reviewed 2026-09-05. The restricted helper,
Polkit/D-Bus activation, deployment client, GUI target controls, package
staging and direct `pkexec` bootstrap from a complete source-tree Release build are
implemented. The GUI now offers an install action when the helper is absent,
reloads systemd and D-Bus, and verifies that the helper responds before
enabling privileged integration actions. The deployment engine has field-aware
PAM parsing plus isolated Arch Plasma, KScreenLocker, Fedora GDM,
Debian/Ubuntu GDM and SDDM transformation/rollback fixtures. Real interactive
Polkit acceptance, full upstream fixture sets, pre-commit PAM validation,
automatic wallet-token eligibility, SELinux policy and cross-distribution VM
acceptance remain open. Transactional validation, interrupted-operation recovery,
downgrade refusal and package removal rollback are implemented. The current
verified snapshot is in `current_status.md`.

## Product Boundary

The native package manager remains responsible for installing the application
itself because the GUI cannot run before it exists. Native packages already
contain the helper, PAM module, systemd units, D-Bus policy and Polkit actions.
After package installation, the user should be able to complete storage
initialization, service activation, desktop integration, enrollment, acceptance
testing and rollback without opening a terminal.

For the developer source-tree Release workflow, a running GUI may bootstrap the
same system components directly. The client resolves the project `xmake.lua`,
fixed installer and a complete set of Release artifacts relative to the running
executable, invokes that installer through `pkexec` without a shell command
string, reloads systemd and D-Bus, and verifies the helper protocol before
exposing further actions. This path is an explicit administrator-authorized
deployment of the current build; production distribution and a future
standalone portable installer remain separate work.

## Goals

- Replace post-install commands with one guided, resumable setup flow.
- Show what system files and services will change before requesting privileges.
- Request administrator authorization only for the selected privileged action.
- Support DMS, KScreenLocker, Plasma Login Manager, GDM and later SDDM through
  typed deployment operations.
- Make failure and rollback first-class UI states rather than terminal-only
  recovery instructions.
- Keep diagnostics useful before setup and after partial or externally managed
  configuration.
- Leave password authentication usable throughout installation, upgrade and
  removal.

## Non-goals

- Do not run the complete GUI as root.
- Do not invoke `sudo`, build shell command strings or open a root terminal.
- Do not extend `su_authd` with package installation, PAM editing or arbitrary
  filesystem mutation. Its authentication boundary must remain small.
- Do not implement a generic root file editor, command runner or package manager.
- Do not automatically log out, reboot, lock the screen or disable another
  display manager.
- Do not silently select a PAM target from desktop environment variables alone.

## Architecture

```text
Slint setup UI (desktop user)
        |
        | typed requests and progress
        v
C++ deployment client
        |
        | system D-Bus + Polkit authorization
        v
su-deploy-helper (root, on-demand, narrow API)
        |
        +-- inspect trusted package resources
        +-- initialize encrypted storage key
        +-- enable/restart su-authd
        +-- plan/apply/rollback supported PAM transformations
        +-- write deployment journal
```

The helper should be D-Bus activated or launched as a short-lived privileged
process after Polkit authorization. It must not share the world-connectable
authentication control socket.

### Typed operations

The initial API should contain only versioned operations such as:

- `GetCapabilities()`;
- `InspectDeployment()`;
- `PlanStorageInitialization()` and `InitializeStorage()`;
- `SetAuthServiceEnabled(bool)` and `RestartAuthService()`;
- `PlanDesktopIntegration(target, options)`;
- `ApplyDesktopIntegration(plan_id)`;
- `RollbackDesktopIntegration(target)`;
- `RemoveManagedDeployment()`.

There is no operation accepting a shell command, arbitrary destination path,
environment override or PAM module name. Plans expire after a short interval
and are bound to the inspected file identity so an apply cannot race an external
edit.

### Privilege separation

Use separate Polkit actions for:

- storage-key initialization;
- authentication-service management;
- PAM desktop integration;
- rollback/removal.

Policies require an active local session and administrator authentication.
Read-only diagnostics stay unprivileged where filesystem permissions allow.
The helper verifies D-Bus peer credentials and never trusts a username supplied
by the GUI as the caller identity.

## Deployment State Model

Replace the current single service/PAM booleans with a typed snapshot:

```text
Package       missing | incompatible | ready
Storage       missing | host-key | tpm2-bound | error
Service       disabled | starting | online | degraded | error
Recognizer    missing | ready | error
Login target  absent | supported | planned | managed | external | conflict
Lock target   absent | supported | planned | managed | external | conflict
Wallet token  unavailable | eligible | enabled | degraded
Enrollment    empty | ready
Acceptance    never-run | passed | rejected | unavailable | stale
Rollback      unavailable | ready | required | failed
```

The snapshot includes stable reason codes and safe display details, not raw
journal output or secret material. GUI localization maps reason codes to text.

The helper stores a root-only deployment journal under
`/var/lib/smile2unlock`. It records package version, target service, original
file identity, backup path, transformation version, resulting hash and service
state. It never stores face data, passwords, LUKS tokens or PAM responses.

## Guided GUI Flow

### 1. System check

- Confirm that the running GUI and installed helper/package versions match.
- Detect systemd, PAM module directory, service unit, models, supported desktop
  entry points, SELinux state and optional wallet-token prerequisites.
- Distinguish missing dependencies from unsupported PAM layouts.
- Offer refresh and a detailed, copyable diagnostic report without requiring
  root privileges.

### 2. Secure storage

- Explain whether TPM2-bound or host-key protection will be selected.
- Request Polkit authorization and initialize the key idempotently.
- Preserve an existing valid credential and refuse unsafe paths or plaintext
  fallback.
- Refresh daemon storage diagnostics after completion.

### 3. Authentication service

- Enable and start `su-authd.service` through the helper.
- Show bounded progress and the daemon's typed readiness result.
- Allow restart and log-safe troubleshooting without exposing secrets.
- Never proceed to PAM changes while the installed module and daemon versions
  are incompatible.

### 4. Desktop targets

- Present login manager and screen locker as separate selections.
- Preselect only targets proven present; require user confirmation before apply.
- Show the effective source file, managed override destination, password
  fallback status and a structured summary of planned changes.
- Offer optional wallet auto-unlock only when the system passes eligibility
  checks from the PAM integration plan.
- For DMS, install the root-owned PAM service through the helper and apply the
  per-user `lockPamPath` setting from the unprivileged GUI process.

### 5. Apply and verify

- Reinspect files immediately before apply and reject stale plans.
- Write changes atomically, validate the resulting PAM service in isolation and
  roll back automatically if validation fails.
- Refresh the GUI snapshot after every operation, including partial failures.
- Run the existing user-level PAM acceptance path from the GUI before offering
  a real lock/login test.

### 6. Enrollment and desktop test

- Continue into camera selection, face enrollment and the existing desktop
  authentication test.
- Persist successful acceptance separately for each relevant package/config
  generation so an upgrade can mark an old result stale.
- Do not mark setup complete merely because a PAM line exists.

### 7. Handoff

- Summarize login, lock, wallet and password-fallback readiness separately.
- Tell the user when logout or reboot is required, but never trigger it without
  a distinct later confirmation.
- Keep rollback available from settings and diagnostics after onboarding.

## Safe PAM Mutation Engine

The GUI and helper reuse one structured PAM planning library. The library must:

- parse fields, bracket controls, include/substack directives and comments;
- resolve `/etc/pam.d` and vendor `/usr/lib/pam.d` precedence;
- match only versioned supported layouts;
- preserve mode, ownership and distribution-specific lines;
- reject symlinks, non-regular files, unsafe ownership and concurrent changes;
- use descriptor-relative access, `O_NOFOLLOW`/`openat2` and atomic replacement;
- lock per target so two GUI instances cannot apply concurrently;
- make install and removal idempotent;
- retain root-only backups and verify hashes before rollback;
- refuse force-overwriting an unknown administrator modification.

Dry-run output is structured data, not a preformatted shell diff. The GUI can
render affected services and semantic changes without exposing a way to edit or
submit arbitrary root content.

## Service And Storage Operations

Replace shell-script internals with reusable typed implementation code where it
improves safety. Existing scripts remain packaging/developer frontends and call
the same helper or library where practical.

The helper must preserve current behavior:

- storage setup selects TPM2-bound protection or the host-key fallback and
  never plaintext;
- an existing credential is retained;
- systemd daemon reload happens only when unit content changed;
- service enable/restart failures return typed errors;
- package installation itself remains side-effect free;
- PAM integration remains an explicit opt-in action.

## Recovery And Removal

The GUI must always expose these independent actions:

- disable face authentication for one login or lock target;
- restore all Smile2Unlock-managed PAM targets;
- restart or disable the authentication service;
- remove managed deployment state while preserving enrolled profiles;
- remove enrolled profiles separately, with explicit destructive confirmation.

Removal order is PAM integration first, then service disablement. The helper
must refuse to remove the module or daemon while a managed PAM reference still
exists. If a file changed externally after setup, show the conflict and preserve
both the backup and current file for administrator recovery rather than guessing.

Native package uninstall scripts may call a noninteractive safe-disable mode,
but must never delete an externally modified PAM file.

## Implementation Phases

### Phase 0: Contracts and threat model

- [ ] Define deployment snapshot, operation, progress and error schemas.
- [x] Threat-model Polkit, D-Bus identity, TOCTOU, symlinks, concurrent GUI
  instances, stale plans and package downgrade.
- [x] Freeze trusted resource paths and ownership requirements.
- [x] Define what remains possible when the helper is absent or incompatible.

### Phase 1: Read-only deployment diagnostics

- [x] Add a deployment controller and typed read-only probes.
- [x] Split service, storage, login, lock and wallet status in the Slint UI.
- [x] Detect effective PAM services without modifying them.
- [ ] Add a diagnostic export that excludes secrets and face/profile contents.

### Phase 2: Privileged helper foundation

- [x] Implement the narrow helper API and D-Bus activation.
- [x] Add granular Polkit actions and active-session checks.
- [x] Add package/helper protocol version negotiation.
- [x] Move storage initialization and service management behind typed methods.
- [ ] Test cancellation, helper crash and authorization denial.

### Phase 3: PAM plan/apply/rollback

- [x] Implement field-aware parsing and isolated fixtures for Arch Plasma,
  KScreenLocker, Fedora GDM, Debian/Ubuntu GDM and SDDM.
- [x] Add openSUSE common-*/postlogin and upstream-variant fixtures, plus
  coverage for comments, control expressions and tab-separated layouts
  (`tests/deploy/deployment.cpp`).
- [ ] Capture full upstream files as versioned fixture data instead of inline
  representative snippets (the added cases stay inline with the existing suite).
- [x] Return dry-run plans with file identities and expiration.
- [x] Apply atomic transformations and record the deployment journal.
- [x] Validate new stacks before commit and automatically roll back failures.
- [x] Add explicit conflict handling for external edits.

### Phase 4: Guided onboarding

- [x] Build the system, storage, service, desktop target, enrollment, acceptance
  and completion steps.
- [ ] Persist resumable non-secret progress and invalidate stale acceptance.
- [ ] Add accessible progress, cancellation and error states.
- [x] Keep diagnostics and rollback reachable even when setup is incomplete.

### Phase 5: Desktop and wallet integrations

- [x] Connect DMS's user setting without root impersonation.
- [x] Add KScreenLocker, Plasma Login Manager and GDM plans from the PAM
  integration document.
- [ ] Add optional LUKS wallet-token eligibility and configuration.
- [x] Add SDDM only after its PAM matrix passes isolated tests.

### Phase 6: Packaging and lifecycle

- [x] Package the helper, D-Bus service, Polkit policies and PAM resources for
  Arch, DEB and RPM outputs.
- [x] Add direct GUI bootstrap for a complete source-tree Release build using `pkexec`,
  followed by systemd/D-Bus reload and helper verification.
- [ ] Package and validate an optional SELinux policy for Fedora.
- [x] Verify ownership, modes, policy identifiers and absence of maintainer
  scripts that silently enable authentication.
- [x] Add safe upgrade, downgrade refusal, rollback and uninstall behavior.
- [x] Keep CLI wrappers for headless recovery using the same typed operations.

### Phase 7: VM acceptance

- [ ] Test complete first-run deployment without terminal commands after native
  package installation.
- [ ] Cover Arch Plasma, Fedora Plasma, Fedora GNOME, Debian/Ubuntu GNOME and
  openSUSE Plasma.
- [ ] Test authorization cancellation, service failure, unknown PAM layout,
  SELinux denial, external edit, power loss during write and helper crash.
- [ ] Verify password login and recovery console access after every failure.

## Test Strategy

### Automated

- PAM parser and transformation golden tests for every supported fixture.
- Property tests for idempotence, install/remove round trips and comment/spacing
  preservation.
- Fake-root filesystem tests for symlinks, ownership, permissions, races,
  atomic writes, backups and stale plans.
- D-Bus/helper tests for authorization, peer identity, malformed requests,
  cancellation and incompatible versions.
- UI state tests for every deployment snapshot and interrupted workflow.
- Package verification for helper, policy, service and trusted-resource layout.

### VM/manual

- Real Polkit prompts from an active local user session.
- Real systemd credential initialization with TPM2 and host-key fallback.
- Real display-manager login, lock-screen unlock and password fallback.
- Wallet/keyring behavior with matching, missing and mismatched LUKS token.
- SELinux enforcing on Fedora.
- Upgrade, rollback and uninstall without a broken PAM reference.

No test phase may modify the developer workstation's PAM configuration merely
because a matching desktop is installed. Live PAM tests require a designated VM
or target host.

## Completion Criteria

- After installing the native package, a normal user can finish supported Linux
  deployment from the GUI without entering terminal commands.
- The GUI shows every privileged action before Polkit authorization and reports
  a typed result afterward.
- No GUI-controlled API can execute arbitrary commands or write arbitrary root
  paths.
- Password fallback is structurally verified before a PAM change is committed.
- Failed, interrupted and repeated operations are safe and idempotent.
- Login, lock and wallet capabilities are reported separately and accurately.
- Every managed PAM change can be rolled back from the GUI or the shared
  headless recovery frontend.
