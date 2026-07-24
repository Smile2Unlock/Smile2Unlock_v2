#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version="$(tr -d '[:space:]' < "${project_dir}/version.txt")"
verify_dir="${project_dir}/build/packages/.verify"

usage() {
    echo "Usage: packaging/linux/verify-package.sh PACKAGE.tar.gz|PACKAGE.deb|PACKAGE.rpm [...]" >&2
}

[[ $# -gt 0 ]] || { usage; exit 64; }

required_paths=(
    usr/bin/su_app
    usr/lib/security/pam_smile2unlock.so
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
)

require_mode() {
    local root="$1"
    local mode="$2"
    local path="$3"
    local actual
    actual="$(stat -c '%a' "${root}/${path}")"
    [[ "$actual" == "$mode" ]] || {
        echo "unexpected mode ${actual} for ${path}; expected ${mode}" >&2
        exit 1
    }
}

verify_tree() {
    local root="$1"
    local path
    for path in "${required_paths[@]}"; do
        [[ -f "${root}/${path}" ]] || {
            echo "missing package path: /${path}" >&2
            exit 1
        }
    done

    for path in \
        usr/bin/su_app \
        usr/libexec/smile2unlock/su_authd \
        usr/libexec/smile2unlock/su_deploy_helper \
        usr/libexec/smile2unlock/install-dms-lock \
        usr/libexec/smile2unlock/setup-storage-key; do
        require_mode "$root" 755 "$path"
    done
    require_mode "$root" 644 usr/lib/security/pam_smile2unlock.so
    require_mode "$root" 644 usr/lib/systemd/system/su-authd.service
    require_mode "$root" 644 usr/lib/systemd/system/su-deploy-helper.service
    require_mode "$root" 644 usr/share/dbus-1/system-services/io.github.smile2unlock.Deployment1.service
    require_mode "$root" 644 usr/share/dbus-1/system.d/io.github.smile2unlock.Deployment1.conf
    require_mode "$root" 644 usr/share/polkit-1/actions/io.github.smile2unlock.deployment.policy
    require_mode "$root" 644 usr/share/smile2unlock/pam/dankshell-smile2unlock

    local app_rpath authd_rpath helper_rpath
    app_rpath="$(patchelf --print-rpath "${root}/usr/bin/su_app")"
    authd_rpath="$(patchelf --print-rpath "${root}/usr/libexec/smile2unlock/su_authd")"
    helper_rpath="$(patchelf --print-rpath "${root}/usr/libexec/smile2unlock/su_deploy_helper")"
    [[ "$app_rpath" == '$ORIGIN/../lib/smile2unlock' ]] || {
        echo "unexpected su_app RPATH: ${app_rpath}" >&2
        exit 1
    }
    [[ "$authd_rpath" == '$ORIGIN/../../lib/smile2unlock' ]] || {
        echo "unexpected su_authd RPATH: ${authd_rpath}" >&2
        exit 1
    }
    [[ -z "$helper_rpath" ]] || {
        echo "unexpected su_deploy_helper RPATH: ${helper_rpath}" >&2
        exit 1
    }
    if [[ "$app_rpath$authd_rpath$helper_rpath" == *"/home/"* \
        || "$app_rpath$authd_rpath$helper_rpath" == *".xmake"* ]]; then
        echo "package contains a development-machine RPATH" >&2
        exit 1
    fi
}

verify_tarball() {
    local package="$1"
    if ! tar --numeric-owner -tvzf "$package" | awk '$2 != "0/0" { exit 1 }'; then
        echo "tarball contains files not owned by root" >&2
        exit 1
    fi
    local root="${verify_dir}/tar-root"
    rm -rf "$root"
    mkdir -p "$root"
    tar -xzf "$package" -C "$root"
    verify_tree "${root}/smile2unlock-${version}"
}

verify_deb() {
    local package="$1"
    command -v dpkg-deb >/dev/null || {
        echo "dpkg-deb is required to verify ${package}" >&2
        exit 1
    }
    [[ "$(dpkg-deb -f "$package" Package)" == "smile2unlock" ]]
    [[ "$(dpkg-deb -f "$package" Version)" == "$version" ]]
    [[ "$(dpkg-deb -f "$package" Architecture)" == "amd64" ]]
    if ! dpkg-deb --contents "$package" \
        | awk '$2 != "root/root" && $2 != "0/0" { exit 1 }'; then
        echo "DEB contains files not owned by root" >&2
        exit 1
    fi

    local root="${verify_dir}/deb-root"
    local control="${verify_dir}/deb-control"
    rm -rf "$root" "$control"
    mkdir -p "$root" "$control"
    dpkg-deb --extract "$package" "$root"
    dpkg-deb --control "$package" "$control"
    if find "$control" -maxdepth 1 -type f \
        \( -name 'preinst' -o -name 'postinst' -o -name 'prerm' -o -name 'postrm' \) \
        | grep -q .; then
        echo "DEB unexpectedly contains maintainer scripts" >&2
        exit 1
    fi
    verify_tree "$root"
}

verify_rpm() {
    local package
    package="$(realpath "$1")"
    command -v rpm rpm2cpio cpio >/dev/null || {
        echo "rpm, rpm2cpio and cpio are required to verify ${package}" >&2
        exit 1
    }
    [[ "$(rpm -qp --queryformat '%{NAME}' "$package")" == "smile2unlock" ]]
    [[ "$(rpm -qp --queryformat '%{VERSION}' "$package")" == "$version" ]]
    [[ "$(rpm -qp --queryformat '%{ARCH}' "$package")" == "x86_64" ]]
    rpm -qlvp "$package" | awk '$3 != "root" || $4 != "root" { exit 1 }'
    [[ -z "$(rpm -qp --scripts "$package")" ]] || {
        echo "RPM unexpectedly contains package scripts" >&2
        exit 1
    }

    local root="${verify_dir}/rpm-root"
    rm -rf "$root"
    mkdir -p "$root"
    (cd "$root" && rpm2cpio "$package" | cpio -idmu --quiet)
    verify_tree "$root"
}

mkdir -p "$verify_dir"
for package in "$@"; do
    [[ -f "$package" ]] || { echo "package not found: ${package}" >&2; exit 1; }
    case "$package" in
        *.tar.gz) verify_tarball "$package" ;;
        *.deb) verify_deb "$package" ;;
        *.rpm) verify_rpm "$package" ;;
        *) echo "unsupported package type: ${package}" >&2; exit 64 ;;
    esac
    echo "verified ${package}"
done
