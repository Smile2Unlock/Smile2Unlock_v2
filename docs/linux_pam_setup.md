# Linux PAM Setup

`su_authd` is a root-owned system service. It starts before the display manager,
owns `/run/smile2unlock/control.sock`, captures from V4L2 on demand, and reads the
target user's enrolled profiles. The PAM module is only a socket client.

## Package the Linux application

Build the Release targets, then create a portable archive or a native package:

```bash
xmake f -m release --with_slint=y --with_seetaface=y
xmake build
packaging/linux/package.sh --format tar.gz
packaging/linux/package.sh --format pacman
```

The package contains `su_app`, `su_authd`, the PAM module, models, language
packs, bundled Slint / SeetaFace runtime libraries, systemd metadata, the
desktop entry, a DMS PAM template and its installer, and licenses. It does not
enable the service or edit `/etc/pam.d`.
For DEB/RPM output, install fpm and use `--format deb` or `--format rpm`; see
[`packaging/linux/README.md`](../packaging/linux/README.md) for dependency
mapping, PAM module directory overrides and staging details.

## Build and install

Configure the release build with the real recognizer enabled, then build through
the project entry point:

```bash
xmake f -m release --with_slint=y --with_zig=y --with_simd=y --with_seetaface=y
xmake build
sudo packaging/install-linux-auth.sh
```

For distributions with a different PAM module directory, override it explicitly:

```bash
sudo PAM_MODULE_DIR=/lib/x86_64-linux-gnu/security packaging/install-linux-auth.sh
```

Package maintainers can validate or stage the complete filesystem layout without
root privileges or `systemctl` side effects:

```bash
DESTDIR=build/test-data/install-root packaging/install-linux-auth.sh
```

On Fedora with SELinux enabled, installation also loads the packaged runtime
socket policy without changing Enforcing mode. It supports the `xdm_t` domain
used by SDDM and Plasma Login Manager. See the
[SELinux packaging notes](../packaging/linux/README.md#fedora-selinux) for policy
scope, manual installation and removal.

The install script deliberately does not edit `/etc/pam.d`. A bad PAM stack can
lock out every login path, and distributions compose these files differently.
Configure installed PAM targets through **Diagnostics → Desktop integration**
in the app, using **Review**, then **Apply** and administrator authorization.
The helper preserves the password fallback and records the original files for
restore. This is the recommended setup path.

## Keyring auto-unlock in the app

Both levels default to **Off**. An administrator enables a source for each login
(or combined login/lock) target; each user separately saves their password.
Lock-only and administrator-authentication targets have no keyring source choice.

1. Open **Diagnostics → Desktop integration** and select a target's keyring source:
   - **Off** leaves keyring unlocking disabled for this target.
   - **Kernel keyring (systemd)** uses the existing `pam_systemd_loadkey` path. It is
     disabled until display-manager keyring inheritance has been verified.
   - **Saved login password** requires **host+tpm2** storage protection and a
     keyring `auth` line (`pam_gnome_keyring.so` or `pam_kwallet5.so`) in the parent
     PAM service after its password-stack anchor. A session-only keyring hook or
     one inside the skipped password stack is insufficient. The app explains
     why an option is unavailable.
2. Select **Review**, then **Apply**, and authorize the helper.
   Managed targets show their deployed source and support the same two-step
   source change directly. **Review restore**, then **Confirm restore**, restores
   their original PAM files.
3. On **Enrollment**, enroll a face sample, then use **Keyring auto-unlock**.
   Enter your login password, which must match your GNOME Keyring or KWallet
   password, select **Save**, and authorize the operation. The password field
   clears immediately, including on failure. Each user must do this for their
   own account. **Update** replaces an enrolled password.
4. **Whenever your login password changes, use Update here with the new
   password**, and ensure that the keyring password matches it. Stale-password
   detection is not implemented.
5. To disable the system level, choose **Off** for each affected target and
   review/apply. To disable your account and delete its saved password, select
   **Turn off**, then **Confirm turn off**, and authorize. Clearing remains
   available when a saved password can no longer be used (for example, after
   changing the deployed source). Rolling back PAM preserves user enrollment;
   use **Turn off** to remove it explicitly.

The user card reports enrollment and missing prerequisites, including missing
face samples, non-TPM storage, or no login target deployed with **Saved login password**.
It is not shown on Windows. Successful app operations and fixture tests do not
replace the real desktop acceptance test below.

## Safe acceptance test

Before logging out or rebooting, invoke the built PAM module against the running
`su_authd` through an isolated PAM config:

```bash
build/linux/x86_64/release/su_pam_acceptance --user "$USER"
```

The tool creates a private service file under `$XDG_RUNTIME_DIR` for a desktop
user (or `/run` for root), calls `pam_start_confdir`, and removes the file on
exit. It does not edit `/etc/pam.d`. A non-root caller can authenticate only
the NSS user matching its own uid. Look at the camera until it reports one of:

- `pam_result=accepted` (exit 0)
- `pam_result=rejected` (exit 2)
- `pam_result=unavailable` (exit 3)
- `pam_result=error` (exit 4)

The default test uses the PAM module from the current build. To validate an
installed module or alternate socket explicitly:

```bash
build/linux/x86_64/release/su_pam_acceptance \
  --user "$USER" \
  --module /usr/lib/security/pam_smile2unlock.so \
  --socket /run/smile2unlock/control.sock
```

Keep an authenticated root shell open while testing the real display manager.
An accepted result proves the PAM module, socket, daemon, target user's data,
camera, models, liveness check, and face comparison worked in one request.

To repeat the multi-user isolation matrix, first build the project, keep the
installed daemon synchronized with that build, and run the root-only harness
with an existing enrolled desktop user as its data source:

```bash
sudo SOURCE_USER="$USER" packaging/testing/multi-user-acceptance.sh
```

The harness creates a PID-suffixed local account, verifies missing data and
cross-user rejection, copies the source enrollment as test-user-owned data,
and checks symlink, owner, mode, corruption, inaccessible-home, and removed-file
failures. It compares the source files' hashes without printing their contents
and removes the account, home, and private runtime directory on exit. This is a
daemon/PAM isolation test; enrollment and deletion in a second graphical desktop
session remain manual checks.

## Enable a PAM entry point

Use the app's **Desktop integration** setup above. The older manual recipe was
to add this immediately before the existing password authentication include:

```text
auth sufficient pam_smile2unlock.so
```

**Warning:** a successful face match on this parent-level `sufficient` line ends
the parent `auth` stack. GNOME Keyring/KWallet `auth` lines after it never run,
so this recipe blocks keyring auto-unlock even when a saved password is available
(issue #129). A managed substack lets those parent wallet lines run after face
authentication. Keyring password delivery still requires an explicitly enabled
source and user enrollment as described above.

The deployment helper can **adopt** an existing manual setup without first
removing it by hand. Its backend review/apply API is available for app integration;
the adoption UI is not yet wired into Desktop integration. Adoption accepts exactly
one `auth sufficient pam_smile2unlock.so` line, with no arguments or only
`socket=/run/smile2unlock/control.sock`, immediately before a recognized password
include/substack. Other Smile2Unlock includes, extra arguments, multiple face
lines, or a different control/order are refused with a reason. DMS uses its
separate installer. Keep the existing password authentication lines. On Arch Linux, console
login commonly includes `system-local-login`; display managers and screen lockers
usually have their own file under `/etc/pam.d`. Check the actual service before
editing it and keep a root shell open while validating the first login.

The explicit default socket is also accepted for adoption:

```text
auth sufficient pam_smile2unlock.so socket=/run/smile2unlock/control.sock
```

Adoption creates the same managed layout as normal configuration in one journaled
transaction. Its token source defaults to `none`; `sealed` requires a parent
wallet `auth` line after the anchor. Restore returns the exact original manual
file bytes (or removes the override for a vendor file). Apply refuses files changed
since review, and restore refuses managed files changed since apply.

For backend callers, `PlanAdoptDesktopIntegration(target_id, authtok_source)` on
`io.github.smile2unlock.Deployment1` returns a plan id and JSON describing the
target, token source, and generated substack. Review that result before explicitly calling
the existing `ApplyDesktopIntegration(plan_id)` on the same D-Bus connection.
Plans expire after two minutes and use the same `configure-pam` Polkit action as
normal setup. `AppController::plan_desktop_adoption` and
`apply_desktop_adoption` provide this two-step flow for the future Linux UI.

## sudo and desktop administrator authorization

The GUI's **Desktop integration** section also detects the `sudo`, `sudo-i`
and `polkit-1` PAM services. Each installed service has its own review, apply
and restore action. These entries are labelled **Administrator authorization**
and do not change the reported login or screen-lock integration state.

The helper replaces only the recognized password authentication include with
a dedicated Smile2Unlock substack. Face success skips the password verifier;
face rejection, a busy camera or an unavailable daemon continues to the
original password stack. Existing parent authentication restrictions, account checks,
sudoers rules, Polkit authorization policies and session setup remain in force.
Restore uses the recorded original files; do not manually delete the substack
reference, since that would also remove its password fallback.

`sudo-i` may include `sudo` rather than a distribution password stack directly;
that reference is preserved. Enabling `sudo` also covers login-shell
authentication when `sudo-i` inherits it. A missing `sudo-i` service is not
created: check the distribution's sudo configuration and `pam_login_service` setting if a
separate login-shell entry is needed. Custom PAM service names are not detected.

Polkit supplies administrator authorization dialogs used by many desktop
applications (including KDE's Polkit agent). It authenticates the administrator
identity chosen by the existing policy, which needs its own enrolled face
profile. sudo likewise authenticates its configured PAM user; `rootpw`,
`targetpw` and `runaspw` can select a different account. This integration does
not unlock SSH private keys or arbitrary application secrets. For login
services, optional [keyring auto-unlock](linux_desktop_pam_integration_plan.md#sealed-per-user-wallet-token)
can supply a saved GNOME Keyring/KWallet password after accepted face
authentication. It is off by default, requires host+TPM2 storage, and is refused
for administrator authorization and lock-only targets. Use **Saved login password**
and the enrollment steps above to enable it. Stale-password detection is not
implemented.

After applying an entry, test without sudo's cached credentials:

```bash
sudo -k
sudo true
sudo -k
sudo -i true
pkexec /usr/bin/true
```

Check face success, rejected recognition with correct/incorrect password, and
password fallback while `su-authd.service` is stopped. Validate the actual
Polkit dialog too; GUI agent timing and Fedora SELinux enforcement require
testing on the installed desktop. Fedora packages install socket permissions
for the confined sudo domains and Polkit PAM helper with the existing runtime
policy. These permissions grant no administrator action by themselves.

## Enable DMS lock-screen authentication

DMS runs its PAM subprocess as the desktop user, so the control socket is
connectable by local users. `su_authd` still authorizes every connection with
`SO_PEERCRED`: root retains status, cancellation, and cross-user authentication;
a non-root peer can only submit an authentication request for the NSS user whose
uid matches its own. Cross-user and non-authentication requests are rejected.

Install the dedicated service from a source checkout:

```bash
sudo packaging/install-dms-lock.sh
```

For an installed package, use:

```bash
sudo /usr/libexec/smile2unlock/install-dms-lock
```

The installer manages only `/etc/pam.d/dankshell-smile2unlock`. It refuses to
overwrite a modified file unless `--force` is given, and a forced replacement
is backed up under `/var/lib/smile2unlock/pam-backups`. It never modifies DMS's
generated PAM file under the user's state directory.

Validate the service and select it as the desktop user running DMS:

```bash
dms auth validate --path /etc/pam.d/dankshell-smile2unlock --json
dms ipc call settings set lockPamPath /etc/pam.d/dankshell-smile2unlock
dms ipc call settings get lockPamPath
dms ipc call settings get loginctlLockIntegration
```

The service uses `pam_smile2unlock.so` as `sufficient`, followed by the complete
system `login` stack. A rejected, timed-out, busy, or unavailable face attempt
therefore continues to DMS password authentication.

Keep DMS's `loginctlLockIntegration` enabled (its default). The desktop app
listens to the current logind session's standard `Lock` signal, stops preview or
an in-progress capture, and releases V4L2 before lock-screen authentication.
The daemon also retries camera acquisition for up to 1.2 seconds. Authentication
uses a six-second budget normally and a twelve-second budget when liveness
detection is enabled. Preview remains stopped after unlock until the user
explicitly starts it again.

Before removing the PAM service, reset DMS to its automatically resolved stack
as the desktop user, then remove the file as root:

```bash
dms ipc call settings set lockPamPath ""
sudo /usr/libexec/smile2unlock/install-dms-lock --remove
```

Keep a working unlocked session while performing the first live lock-screen
test. Confirm face success, password fallback after a failed face attempt, and
password fallback while `su-authd.service` is stopped.

## User data

At boot, the service resolves the PAM username through NSS and uses:

```text
~/.config/smile2unlock/config.toml
/var/lib/smile2unlock/users/<uid>/profiles.s2u
```

The config remains a user-owned regular file and must not be group- or
world-writable. The profile store is a root-owned, per-UID XChaCha20-Poly1305
envelope created only through the daemon control protocol; the GUI never reads
the master key or encrypted file directly. The daemon resolves the username
through NSS for every request. An NSS backend error or an empty/non-absolute
home is reported as unavailable, and deleted users are not retained in a
daemon-side identity cache.

On Linux, the daemon opens the NSS home and config path with `openat2`, pins the
regular file by descriptor, and gives Rust only the pinned `/proc/self/fd` path.
Service environments that filter `openat2` use a descriptor-relative
`openat(O_NOFOLLOW)` traversal instead. Config files larger than 1 MiB and
encrypted profile stores larger than 16 MiB are rejected.

Homes that remain encrypted and unavailable until after password authentication
cannot supply the per-user recognition policy during initial login. Face
profiles no longer depend on the home because they are system-owned, but the
service still fails closed while the required user config is unavailable.

Authentication starts are limited to one per uid per second. The limit is
memory-only and returns PAM unavailable so the existing password stack can run;
it does not count failures or lock accounts and is intentionally independent of
distribution `pam_faillock` policy. A failed model initialization is retried
lazily after five seconds, while cameras are enumerated and opened for each
request.

## Diagnostics

```bash
systemctl status su-authd.service
journalctl -u su-authd.service -b
ls -l /run/smile2unlock/control.sock
```

The expected socket mode is `0666`; this permits DMS's user-owned PAM process to
connect. Authorization is enforced from kernel-provided peer credentials in the
daemon, not from the socket mode.

Each handled request emits `request_started` and `request_completed` records.
The completion record includes the result, fixed diagnostic reason, and elapsed
milliseconds, but never includes images, embeddings, or profile JSON.
