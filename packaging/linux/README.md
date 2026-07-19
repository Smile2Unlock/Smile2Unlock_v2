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

The archive contains `su_app`, `su_authd`, the PAM module, bundled Slint and
SeetaFace libraries, models, language packs, the systemd unit, desktop entry,
icon, DMS lock-screen PAM template and installer, and license files.

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

The package layout is the same for every format. fpm dependencies default to
`pam`; set `PACKAGE_DEPENDS` to the names used by the target distribution when
packaging the system libraries required by Slint, libyuv, libjpeg, OpenMP and
libsystemd:

```bash
PACKAGE_DEPENDS='libpam0g,libyuv0,libjpeg8,libgomp1,libsystemd0' \
  packaging/linux/package.sh --format deb
```

If the target distribution uses a multiarch PAM directory, override it while
building the package:

```bash
PAM_MODULE_DIR=/lib/x86_64-linux-gnu/security \
  packaging/linux/package.sh --format deb
```

The package never enables `su-authd.service` and never edits `/etc/pam.d`.
Install the package first, then follow [linux_pam_setup.md](../../docs/linux_pam_setup.md)
to enable and validate the desired PAM entry point.

## DMS lock screen

After installing the package and starting `su-authd.service`, install the
dedicated PAM service as root:

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
