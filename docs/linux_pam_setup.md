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

The install script deliberately does not edit `/etc/pam.d`. A bad PAM stack can
lock out every login path, and distributions compose these files differently.

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

## Enable a PAM entry point

Add this before the password module in the PAM service that should allow face
authentication:

```text
auth sufficient pam_smile2unlock.so
```

Keep the existing password authentication lines after it. On Arch Linux, console
login commonly includes `system-local-login`; display managers and screen lockers
usually have their own file under `/etc/pam.d`. Check the actual service before
editing it and keep a root shell open while validating the first login.

An alternate socket can be supplied for diagnostics:

```text
auth sufficient pam_smile2unlock.so socket=/run/smile2unlock/control.sock
```

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
```

The service uses `pam_smile2unlock.so` as `sufficient`, followed by the complete
system `login` stack. A rejected, timed-out, busy, or unavailable face attempt
therefore continues to DMS password authentication.

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

At boot, the service resolves the PAM username through NSS and reads:

```text
~/.config/smile2unlock/config.toml
~/.local/share/smile2unlock/profiles.json
```

Both files must be regular files owned by that user and must not be group- or
world-writable. The profile store must already exist from enrollment in `su_app`.

Homes that remain encrypted and unavailable until after password authentication
cannot supply their face profile during initial login. Supporting that setup
requires moving encrypted profile data and key management into a system-owned
store; the service fails closed when the home data is unavailable.

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
