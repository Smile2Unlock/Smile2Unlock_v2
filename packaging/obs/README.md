# Open Build Service packaging

Project: https://build.opensuse.org/project/show/home:ation_ciger:Smile2Unlock

The first OBS integration repackages the checksum-verified upstream Linux
release into native DEB, RPM and Arch packages. It **does not build the
application from source**. The current source build needs GCC 16/C++26 and
network-dependent Xmake/Rust packages; full offline source builds need a
separate vendoring and toolchain effort.

Targets are x86_64: Debian 13, Ubuntu 24.04/26.04, Fedora 43/44, openSUSE
Tumbleweed/Leap 16.0 and Arch Extra. Build success is not desktop/PAM acceptance
testing. The release's actual PAM glibc requirement is read from its runtime
inventory; 2.3.3 requires glibc >= 2.38. Debian 12, Ubuntu 22.04 and Leap 15
are therefore excluded.

## Generate and verify build inputs

Download the release's portable Linux tarball and `SHA256SUMS`, verify the
tarball, then generate a small directory of OBS inputs:

```sh
python3 packaging/obs/generate.py \
  --version 2.3.3 \
  --archive /tmp/smile2unlock-2.3.3-linux-x86_64.tar.gz \
  --sha256 86f0ea244dfe50c8b2290a085d878a4165a546399e7bba32394f21b8559d1777 \
  --output build/obs-inputs
```

OBS's source service downloads the archive **before** entering its isolated
builder and checks SHA256. Each native build checks the payload manifest before
copying it, chooses the distro PAM directory, and updates that manifest's path.
Bundled ELF files are not stripped or advertised as system-library providers.
Package installation preserves the upstream hooks: no automatic PAM editing
or enabling authentication, downgrade checks before upgrade, and transactional
PAM rollback before final removal. The existing Fedora SELinux policy still
requires acceptance testing on other SELinux distributions.

## GitHub release sync

`Sync Linux release to OBS` runs after the existing release artifact workflow
succeeds, or manually for a published stable tag. It verifies the upstream
checksum and publishes build inputs to the dedicated `obs-releases`
branch, without adding the 200 MB binary archive to Git.

Set the OBS package SCM Sync URL to:

```text
https://github.com/Smile2Unlock/Smile2Unlock_v2.git#obs-releases
```

Create an OBS **service** token restricted to project
`home:ation_ciger:Smile2Unlock` and package `smile2unlock`, then save it as the
GitHub repository Actions secret `OBS_SERVICE_TOKEN`. The workflow uses it
only to refresh this package. No GitHub personal access token is sent to OBS.
Without the token, publication succeeds but the workflow fails with an
explicit configuration message instead of silently leaving OBS stale.

Reference: https://openbuildservice.org/help/manuals/obs-user-guide/cha.obs.source_service.html
