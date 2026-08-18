#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/common.sh
source "${script_dir}/../lib/common.sh"

[[ $# -gt 0 ]] || package_die \
    "usage: packaging/linux/verify-package.sh PACKAGE.tar.gz|PACKAGE.deb|PACKAGE.rpm|PACKAGE.pkg.tar.zst [...]"
package_require_command patchelf

required_paths=(
    usr/bin/su_app
    usr/lib/smile2unlock/libslint_cpp.so
    usr/lib/smile2unlock/libSeetaFaceDetector600.so
    usr/libexec/smile2unlock/su_authd
    usr/libexec/smile2unlock/su_deploy_helper
    usr/libexec/smile2unlock/install-dms-lock
    usr/libexec/smile2unlock/setup-storage-key
    usr/lib/systemd/system/su-authd.service
    usr/lib/systemd/system/su-deploy-helper.service
    usr/share/dbus-1/system-services/io.github.smile2unlock.Deployment1.service
    usr/share/dbus-1/system.d/io.github.smile2unlock.Deployment1.conf
    usr/share/polkit-1/actions/io.github.smile2unlock.deployment.policy
    usr/share/applications/smile2unlock.desktop
    usr/share/icons/hicolor/128x128/apps/smile2unlock.png
    usr/share/smile2unlock/i18n/en.json
    usr/share/smile2unlock/i18n/zh-CN.json
    usr/share/smile2unlock/models/face_detector.csta
    usr/share/smile2unlock/models/face_landmarker_pts5.csta
    usr/share/smile2unlock/models/face_recognizer.csta
    usr/share/smile2unlock/models/fas_first.csta
    usr/share/smile2unlock/models/fas_second.csta
    usr/share/smile2unlock/pam/dankshell-smile2unlock
    usr/share/smile2unlock/release-info.json
)

require_mode() {
    local root="$1" expected="$2" path="$3"
    local actual
    actual="$(stat -c '%a' "${root}/${path}")"
    [[ "$actual" == "$expected" ]] \
        || package_die "unexpected mode ${actual} for /${path}; expected ${expected}"
}

verify_tree() {
    local root="$1"
    local path
    for path in "${required_paths[@]}"; do
        package_require_file "${root}/${path}"
    done

    mapfile -t pam_modules < <(find "$root" -type f -path '*/security/pam_smile2unlock.so' -print)
    (( ${#pam_modules[@]} == 1 )) || package_die "expected exactly one PAM module"

    for path in usr/bin/su_app usr/libexec/smile2unlock/su_authd \
        usr/libexec/smile2unlock/su_deploy_helper \
        usr/libexec/smile2unlock/install-dms-lock \
        usr/libexec/smile2unlock/setup-storage-key; do
        require_mode "$root" 755 "$path"
    done
    [[ "$(stat -c '%a' "${pam_modules[0]}")" == 755 ]] \
        || package_die "PAM module must have mode 755"
    for path in usr/lib/systemd/system/su-authd.service \
        usr/lib/systemd/system/su-deploy-helper.service \
        usr/share/dbus-1/system-services/io.github.smile2unlock.Deployment1.service \
        usr/share/dbus-1/system.d/io.github.smile2unlock.Deployment1.conf \
        usr/share/polkit-1/actions/io.github.smile2unlock.deployment.policy \
        usr/share/smile2unlock/pam/dankshell-smile2unlock; do
        require_mode "$root" 644 "$path"
    done

    local app_rpath authd_rpath helper_rpath
    app_rpath="$(patchelf --print-rpath "${root}/usr/bin/su_app")"
    authd_rpath="$(patchelf --print-rpath "${root}/usr/libexec/smile2unlock/su_authd")"
    helper_rpath="$(patchelf --print-rpath "${root}/usr/libexec/smile2unlock/su_deploy_helper")"
    [[ "$app_rpath" == '$ORIGIN/../lib/smile2unlock' ]] \
        || package_die "unexpected su_app RPATH: ${app_rpath}"
    [[ "$authd_rpath" == '$ORIGIN/../../lib/smile2unlock' ]] \
        || package_die "unexpected su_authd RPATH: ${authd_rpath}"
    [[ -z "$helper_rpath" ]] || package_die "unexpected su_deploy_helper RPATH: ${helper_rpath}"

    if [[ "$app_rpath$authd_rpath$helper_rpath" == *'/home/'* \
        || "$app_rpath$authd_rpath$helper_rpath" == *'.xmake'* ]]; then
        package_die "package contains a development-machine RPATH"
    fi

    local binary unresolved
    for binary in "${root}/usr/bin/su_app" "${root}/usr/libexec/smile2unlock/su_authd"; do
        unresolved="$(LD_LIBRARY_PATH="${root}/usr/lib/smile2unlock" ldd "$binary" \
            | sed -n 's/^[[:space:]]*\([^[:space:]]*\) => not found.*/\1/p')"
        [[ -z "$unresolved" ]] || package_die \
            "unresolved runtime dependencies for ${binary#$root}: ${unresolved//$'\n'/, }"
    done

    package_verify_release_info "$root" \
        "${root}/usr/share/smile2unlock/release-info.json"
}

verify_tar_paths() {
    local archive="$1"
    while IFS= read -r path; do
        [[ "$path" != /* && "$path" != ../* && "$path" != */../* ]] \
            || package_die "unsafe archive path: $path"
    done < <(tar -tf "$archive")
}

verify_tarball() {
    local archive="$1"
    verify_tar_paths "$archive"
    tar --numeric-owner -tvzf "$archive" | awk '$2 != "0/0" { exit 1 }' \
        || package_die "tarball contains files not owned by root"
    local root="${verify_root}/tar"
    mkdir -p "$root"
    tar -xzf "$archive" -C "$root"
    verify_tree "${root}/${package_name}-${package_version}"
}

verify_deb() {
    local archive="$1"
    package_require_command dpkg-deb
    [[ "$(dpkg-deb -f "$archive" Package)" == "$package_name" ]] \
        || package_die "unexpected DEB package name"
    [[ "$(dpkg-deb -f "$archive" Version)" == "$package_version" ]] \
        || package_die "unexpected DEB version"
    local root="${verify_root}/deb"
    mkdir -p "$root"
    dpkg-deb --extract "$archive" "$root"
    verify_tree "$root"
}

verify_rpm() {
    local archive="$1"
    package_require_command rpm
    package_require_command rpm2cpio
    package_require_command cpio
    [[ "$(rpm -qp --queryformat '%{NAME}' "$archive")" == "$package_name" ]] \
        || package_die "unexpected RPM package name"
    [[ "$(rpm -qp --queryformat '%{VERSION}' "$archive")" == "$package_version" ]] \
        || package_die "unexpected RPM version"
    local root="${verify_root}/rpm"
    mkdir -p "$root"
    (cd "$root" && rpm2cpio "$archive" | cpio -idmu --quiet)
    verify_tree "$root"
}

verify_pacman() {
    local archive="$1"
    package_require_command tar
    verify_tar_paths "$archive"
    local root="${verify_root}/pacman"
    mkdir -p "$root"
    tar -xf "$archive" -C "$root"
    verify_tree "$root"
}

for archive_arg in "$@"; do
    archive="$(package_absolute_path "$archive_arg")"
    package_require_file "$archive"
    package_reset_stage "${project_dir}/build/package-stage/verify-linux"
    verify_root="${project_dir}/build/package-stage/verify-linux"
    case "$archive" in
        *.pkg.tar.zst) verify_pacman "$archive" ;;
        *.tar.gz) verify_tarball "$archive" ;;
        *.deb) verify_deb "$archive" ;;
        *.rpm) verify_rpm "$archive" ;;
        *) package_die "unsupported package type: $archive" ;;
    esac
    echo "verified ${archive}"
done
