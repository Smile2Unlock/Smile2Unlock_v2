# Linux Packaging

`packaging/linux/package.sh` builds one common filesystem staging tree and can
produce a portable archive, an Arch package, or DEB/RPM packages through fpm.
The staging tree is created under `build/packages`; it does not write to `/usr`
or modify PAM configuration.

## Build

Build the real Linux application first:

```bash
xmake f -m release --with_slint=y --with_seetaface=y
xmake build
```

Create the portable archive:

```bash
packaging/linux/package.sh --format tar.gz
```

The archive contains `su_app`, `su_authd`, the restricted deployment helper,
the PAM module, bundled Slint and SeetaFace libraries, models, language packs,
systemd, D-Bus and Polkit integration, the desktop entry, DMS PAM resources,
and license files.

## Native packages

On Arch Linux, `makepkg` is used directly:

```bash
packaging/linux/package.sh --format pacman
```

For DEB or RPM output, install [fpm](https://github.com/jordansissel/fpm)
and select the format:

```bash
packaging/linux/package.sh --format deb
packaging/linux/package.sh --format rpm
```

The package layout is the same for every format. DEB dependencies default to
`libc6,libstdc++6,libpam0g,libyuv0,libjpeg62-turbo,libgomp1,libsystemd0,dbus,polkitd`;
RPM dependencies default to
`glibc,libstdc++,pam,libyuv,libjpeg-turbo,libgomp,systemd-libs,dbus,polkit`. Override
`PACKAGE_DEPENDS` when a target distribution uses different package names:

```bash
PACKAGE_DEPENDS='libc6,libstdc++6,libpam0g,libyuv0,libjpeg8,libgomp1,libsystemd0' \
  packaging/linux/package.sh --format deb
```

Build each native package on the oldest release in its supported distribution
family. The packaged executables still use the build environment's glibc ABI;
an Arch-built binary is not expected to run on an older Debian or Fedora
release merely because it was wrapped in a DEB or RPM.

If the target distribution uses a multiarch PAM directory, override it while
building the package:

```bash
PAM_MODULE_DIR=/lib/x86_64-linux-gnu/security \
  packaging/linux/package.sh --format deb
```

The package never enables `su-authd.service` and never edits `/etc/pam.d`.
Install the package, open `su_app`, then use the Desktop Integration section to
initialize storage and configure a detected login or lock-screen target. The
GUI requests administrator authorization through the restricted deployment
helper and shows password fallback before applying a PAM change.

When running `su_app` directly from a complete Release build, the same section
can bootstrap missing system components through `pkexec`. The GUI accepts only
the installer and artifacts found beside that build, requests administrator
authorization, reloads systemd and D-Bus, and verifies the helper response
before enabling integration actions. An installed application with missing
package files instead asks the user to repair the native package.

For headless recovery, storage initialization remains available directly:

```bash
sudo /usr/libexec/smile2unlock/setup-storage-key
sudo systemctl enable --now su-authd.service
```

The setup command preserves an existing key. It selects TPM2-bound protection
when TPM2 is available and the systemd host-key fallback otherwise; it never
falls back to plaintext. Follow [linux_pam_setup.md](../../docs/linux_pam_setup.md)
for manual diagnostics or recovery.

## DMS lock screen

The GUI detects the DMS command-line API and configures both the root-owned PAM
service and the current user's `lockPamPath`. The following commands are kept
for headless recovery:

```bash
sudo /usr/libexec/smile2unlock/install-dms-lock
```

Then validate and select it as the desktop user running DMS:

```bash
dms auth validate --path /etc/pam.d/dankshell-smile2unlock --json
dms ipc call settings set lockPamPath /etc/pam.d/dankshell-smile2unlock
```

The dedicated service tries face authentication first and always retains the
system `login` password stack as fallback. See `linux_pam_setup.md` in the
package documentation for verification and rollback commands.

## Runtime dependencies

The package bundles Slint and SeetaFace libraries, but intentionally uses the
distribution's system libraries for PAM, libyuv, libjpeg, OpenMP, libc++
runtime, libsystemd, V4L2 and the Wayland / X11 platform stack. The exact
package names are distribution-specific, which is why DEB/RPM dependency names
are configurable.

The script patches application RPATHs to package-relative locations and fails
if a binary still references the build user's Xmake cache.

## Verify packages

Verify generated native packages on the matching distribution family:

```bash
packaging/linux/verify-package.sh build/packages/smile2unlock-2.1.3.deb
packaging/linux/verify-package.sh build/packages/smile2unlock-2.1.3.rpm
packaging/linux/verify-package.sh build/packages/smile2unlock-2.1.3-x86_64.tar.gz
```

DEB verification requires `dpkg-deb` and `patchelf`. RPM verification requires
`rpm`, `rpm2cpio`, `cpio`, and `patchelf`. The verifier checks package metadata,
root ownership, required files, executable and configuration modes, relative
RPATHs, and the absence of maintainer scripts. It extracts only under
`build/packages/.verify`.
