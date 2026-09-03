# Packaging

`packaging/package.sh` is the supported entry point for release packages. It
uses one version, output directory, release metadata format, and verification
policy for Linux and Windows.

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
contains `su_credential_provider.dll` with no version or hash suffix. Running
deployment from the GUI elevates `su_deploy_helper.exe`, copies the trusted
runtime set into `C:\Program Files\Smile2Unlock\bin`, and registers both the
Credential Provider and service against that protected, stable location. The
directory used to extract the zip is not written into either registration.

## Linux layout

All Linux formats are generated from one FHS staging tree under
`build/package-stage/linux/root`. Packaging never enables the auth service or
edits PAM configuration. Those security-sensitive operations remain explicit
GUI actions through the restricted D-Bus/Polkit deployment helper.
