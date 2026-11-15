# Packaging

[Open Build Service integration](obs/README.md) publishes native Linux packages
from verified GitHub release binaries across supported distributions, with a
dedicated packaging branch and an optional package-scoped refresh token.

`packaging/package.sh` is the supported entry point for release packages. It
uses one version, output directory, release metadata format, and verification
policy for Linux and Windows.

## Product version

The pushed `vMAJOR.MINOR.PATCH` tag is the release version authority. The
release workflow validates the tag and passes its numeric version as
`SU_VERSION` to Xmake and both packaging jobs, so executable metadata, package
names, and `release-info.json` use the same version. A release does not require
a version-bump commit or a version-only PR, which keeps release numbering
independent of protected-branch edits.

For local development builds, `version.txt` remains a fallback baseline when
`SU_VERSION` is unset. To build or package a release candidate locally, set the
version explicitly:

```bash
SU_VERSION=2.4.2 xmake f -y -p linux -a x86_64 -m release
SU_VERSION=2.4.2 xmake build
SU_VERSION=2.4.2 packaging/package.sh --platform all
```

Create and push `v2.4.2` only after the intended commit is on `main`; the tag
starts the release workflow. Rebuild before packaging so executable metadata
matches the package version. `version.txt` is not edited as part of a release.
The Rust crates' internal package versions and the core ABI version describe
their own interfaces; they are independent of the product release version.

## Build and package

Package the existing release builds:

```bash
packaging/package.sh --platform all
```

Configure and rebuild both platforms before packaging:

```bash
packaging/package.sh --platform all --build
```

The default outputs are:

- `smile2unlock-<version>-linux-x86_64.tar.gz`
- `smile2unlock-<version>-windows-x86_64.zip`

Linux also supports `--linux-format pacman`, `deb`, `rpm`, or `all`. Native
formats require their platform tools (`makepkg` or `fpm`) and are expected to
be built on the oldest supported member of the target distribution family.

Every archive is verified after creation. Verification covers required files,
the embedded `release-info.json` checksums, package-relative ELF RPATHs and
runtime resolution on Linux, and PE architecture plus recursively imported
non-system DLLs on Windows.

Packaging never modifies tracked repository files. Each archive carries its
own `release-info.json` with version, platform, architecture, and checksums.

## Windows layout

The zip has a stable `Smile2Unlock/` top-level directory. Its `bin/` directory
contains `Smile2UnlockCredentialProvider.dll` with no version or hash suffix. Running
deployment from the GUI elevates `Smile2UnlockDeployHelper.exe`, copies the trusted
runtime set into `C:\Program Files\Smile2Unlock\bin`, and registers both the
Credential Provider and service against that protected, stable location. The
directory used to extract the zip is not written into either registration.

## Linux layout

All Linux formats are generated from one FHS staging tree under
`build/package-stage/linux/root`. Packaging never enables the auth service or
edits PAM configuration. Those security-sensitive operations remain explicit
GUI actions through the restricted D-Bus/Polkit deployment helper.
