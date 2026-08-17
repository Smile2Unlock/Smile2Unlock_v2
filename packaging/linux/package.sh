#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${BUILD_DIR:-${project_dir}/build/linux/x86_64/release}"
output_dir="${OUTPUT_DIR:-${project_dir}/build/packages}"
format="${PACKAGE_FORMAT:-tar.gz}"
pam_module_dir="${PAM_MODULE_DIR:-/usr/lib/security}"
package_name="smile2unlock"
version="$(tr -d '[:space:]' < "${project_dir}/version.txt")"
architecture="$(uname -m)"
work_dir="${project_dir}/build/packages/.work/${package_name}-${version}"
root_dir="${work_dir}/root"

usage() {
    cat <<'USAGE'
Usage: packaging/linux/package.sh [options]

Options:
  --format FORMAT       tar.gz, pacman, deb, rpm, or all (default: tar.gz)
  --build-dir DIR       Release build directory
  --output-dir DIR      Package output directory
  --help                Show this help

Environment:
  PACKAGE_DEPENDS      Override comma-separated fpm dependencies for deb/rpm
USAGE
}

while (($# > 0)); do
    case "$1" in
        --format)
            [[ $# -ge 2 ]] || { echo "missing value for --format" >&2; exit 64; }
            format="$2"
            shift 2
            ;;
        --build-dir)
            [[ $# -ge 2 ]] || { echo "missing value for --build-dir" >&2; exit 64; }
            build_dir="$2"
            shift 2
            ;;
        --output-dir)
            [[ $# -ge 2 ]] || { echo "missing value for --output-dir" >&2; exit 64; }
            output_dir="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage >&2
            exit 64
            ;;
    esac
done

case "$format" in
    tar.gz|pacman|deb|rpm|all) ;;
    *)
        echo "unsupported format: $format" >&2
        exit 64
        ;;
esac

[[ "$pam_module_dir" == /* ]] || {
    echo "PAM_MODULE_DIR must be an absolute path" >&2
    exit 64
}

require_file() {
    [[ -f "$1" ]] || {
        echo "missing build artifact: $1" >&2
        exit 1
    }
}

require_file "${build_dir}/su_app"
require_file "${build_dir}/su_authd"
require_file "${build_dir}/su_deploy_helper"
require_file "${build_dir}/pam_smile2unlock.so"
require_file "${build_dir}/assets/i18n/en.json"
require_file "${build_dir}/assets/i18n/zh-CN.json"
for model in face_detector.csta face_landmarker_pts5.csta face_recognizer.csta fas_first.csta fas_second.csta; do
    require_file "${build_dir}/assets/models/seeta/${model}"
done

command -v patchelf >/dev/null || {
    echo "patchelf is required to make package RPATHs relocatable" >&2
    exit 1
}

rm -rf "${work_dir}"
mkdir -p \
    "${root_dir}/usr/bin" \
    "${root_dir}/usr/libexec/smile2unlock" \
    "${root_dir}${pam_module_dir}" \
    "${root_dir}/usr/lib/smile2unlock" \
    "${root_dir}/usr/lib/systemd/system" \
    "${root_dir}/usr/share/dbus-1/system-services" \
    "${root_dir}/usr/share/dbus-1/system.d" \
    "${root_dir}/usr/share/polkit-1/actions" \
    "${root_dir}/usr/share/applications" \
    "${root_dir}/usr/share/icons/hicolor/128x128/apps" \
    "${root_dir}/usr/share/smile2unlock/i18n" \
    "${root_dir}/usr/share/smile2unlock/models" \
    "${root_dir}/usr/share/smile2unlock/pam" \
    "${root_dir}/usr/share/doc/smile2unlock" \
    "${output_dir}"

install -m 0755 "${build_dir}/su_app" "${root_dir}/usr/bin/su_app"
install -m 0755 "${build_dir}/su_authd" \
    "${root_dir}/usr/libexec/smile2unlock/su_authd"
install -m 0755 "${build_dir}/su_deploy_helper" \
    "${root_dir}/usr/libexec/smile2unlock/su_deploy_helper"
install -m 0755 "${project_dir}/packaging/install-dms-lock.sh" \
    "${root_dir}/usr/libexec/smile2unlock/install-dms-lock"
install -m 0755 "${project_dir}/packaging/setup-storage-key.sh" \
    "${root_dir}/usr/libexec/smile2unlock/setup-storage-key"
install -m 0644 "${build_dir}/pam_smile2unlock.so" \
    "${root_dir}${pam_module_dir}/pam_smile2unlock.so"
install -m 0644 "${project_dir}/packaging/systemd/su-authd.service" \
    "${root_dir}/usr/lib/systemd/system/su-authd.service"
install -m 0644 "${project_dir}/packaging/systemd/su-deploy-helper.service" \
    "${root_dir}/usr/lib/systemd/system/su-deploy-helper.service"
install -m 0644 "${project_dir}/packaging/dbus/io.github.smile2unlock.Deployment1.service" \
    "${root_dir}/usr/share/dbus-1/system-services/io.github.smile2unlock.Deployment1.service"
install -m 0644 "${project_dir}/packaging/dbus/io.github.smile2unlock.Deployment1.conf" \
    "${root_dir}/usr/share/dbus-1/system.d/io.github.smile2unlock.Deployment1.conf"
install -m 0644 "${project_dir}/packaging/polkit/io.github.smile2unlock.deployment.policy" \
    "${root_dir}/usr/share/polkit-1/actions/io.github.smile2unlock.deployment.policy"
install -m 0644 "${project_dir}/packaging/linux/smile2unlock.desktop" \
    "${root_dir}/usr/share/applications/smile2unlock.desktop"
install -m 0644 "${project_dir}/assets/icons/Smile2Unlock.png" \
    "${root_dir}/usr/share/icons/hicolor/128x128/apps/smile2unlock.png"
install -m 0644 "${project_dir}/LICENSE" \
    "${root_dir}/usr/share/doc/smile2unlock/LICENSE"
install -m 0644 "${project_dir}/NOTICE/THIRD-PARTY-NOTICES.md" \
    "${root_dir}/usr/share/doc/smile2unlock/THIRD-PARTY-NOTICES.md"
install -m 0644 "${project_dir}/packaging/linux/README.md" \
    "${root_dir}/usr/share/doc/smile2unlock/README.md"
install -m 0644 "${project_dir}/docs/linux_pam_setup.md" \
    "${root_dir}/usr/share/doc/smile2unlock/linux_pam_setup.md"
install -m 0644 "${project_dir}/packaging/linux/pam/dankshell-smile2unlock" \
    "${root_dir}/usr/share/smile2unlock/pam/dankshell-smile2unlock"

install -m 0644 "${build_dir}/assets/i18n/"*.json \
    "${root_dir}/usr/share/smile2unlock/i18n/"
install -m 0644 "${build_dir}/assets/models/seeta/"*.csta \
    "${root_dir}/usr/share/smile2unlock/models/"

mapfile -t rpath_dirs < <(
    readelf -d "${build_dir}/su_app" \
        | sed -n 's/.*Library rpath: \[\(.*\)\]/\1/p' \
        | tr ':' '\n'
)
slint_dir=""
seeta_dir=""
for directory in "${rpath_dirs[@]}"; do
    if [[ -z "$slint_dir" && -f "${directory}/libslint_cpp.so" ]]; then
        slint_dir="$directory"
    fi
    if [[ -z "$seeta_dir" && -f "${directory}/libSeetaFaceDetector600.so" ]]; then
        seeta_dir="$directory"
    fi
done
[[ -n "$slint_dir" ]] || { echo "failed to locate Slint runtime library" >&2; exit 1; }
[[ -n "$seeta_dir" ]] || { echo "failed to locate SeetaFace runtime libraries" >&2; exit 1; }

shopt -s nullglob
runtime_libraries=(
    "${slint_dir}/libslint_cpp.so"*
    "${seeta_dir}/libSeeta"*.so*
    "${seeta_dir}/libtennis"*.so*
)
shopt -u nullglob
(( ${#runtime_libraries[@]} > 0 )) || { echo "no runtime libraries found" >&2; exit 1; }
install -m 0755 "${runtime_libraries[@]}" "${root_dir}/usr/lib/smile2unlock/"

for library in "${root_dir}/usr/lib/smile2unlock/"*.so*; do
    patchelf --set-rpath '$ORIGIN' "$library"
done
patchelf --set-rpath '$ORIGIN/../lib/smile2unlock' "${root_dir}/usr/bin/su_app"
patchelf --set-rpath '$ORIGIN/../../lib/smile2unlock' \
    "${root_dir}/usr/libexec/smile2unlock/su_authd"

for binary in "${root_dir}/usr/bin/su_app" \
    "${root_dir}/usr/libexec/smile2unlock/su_authd"; do
    if readelf -d "$binary" | grep -E '/home/|/\.xmake/' >/dev/null; then
        echo "package still contains a development-machine RPATH: $binary" >&2
        exit 1
    fi
done

package_root="${work_dir}/${package_name}-${version}"
rm -rf "$package_root"
mkdir -p "$(dirname "$package_root")"
cp -a "${root_dir}" "$package_root"

build_tarball() {
    local output="${output_dir}/${package_name}-${version}-${architecture}.tar.gz"
    tar -C "${work_dir}" --owner=0 --group=0 -czf "$output" "${package_name}-${version}"
    echo "created ${output}"
}

build_pacman() {
    command -v makepkg >/dev/null || {
        echo "makepkg is required for pacman output" >&2
        exit 1
    }
    local pkgbuild="${work_dir}/PKGBUILD"
    cat > "$pkgbuild" <<EOF
pkgname=${package_name}
pkgver=${version}
pkgrel=1
pkgdesc='Local face authentication enrollment and diagnostics'
arch=('${architecture}')
license=('MIT')
options=('!debug')
depends=('pam' 'systemd-libs' 'dbus' 'polkit' 'gcc-libs')
_stage_root='${root_dir}'

package() {
    install -d "\$pkgdir"
    cp -r --no-preserve=ownership "\$_stage_root"/* "\$pkgdir"/
}
EOF
    local makepkg_config="${work_dir}/makepkg.conf"
    {
        printf 'source /etc/makepkg.conf\n'
        printf 'PKGDEST=%q\n' "$output_dir"
    } > "$makepkg_config"
    (cd "$work_dir" && makepkg --force --nodeps --config "$makepkg_config" -p PKGBUILD)
}

build_fpm() {
    local target="$1"
    command -v fpm >/dev/null || {
        echo "fpm is not installed; skipping ${target} output" >&2
        return 2
    }
    local default_depends
    case "$target" in
        deb)
            # libyuv and libjpeg-turbo are linked statically (see xmake.lua),
            # so the package needs no libyuv0/libjpeg system dependency.
            default_depends="libc6,libstdc++6,libpam0g,libgomp1,libsystemd0,dbus,polkitd"
            ;;
        rpm)
            default_depends="glibc,libstdc++,pam,libgomp,systemd-libs,dbus,polkit"
            ;;
    esac
    local -a depends=()
    local dependency
    IFS=',' read -r -a configured_depends <<< "${PACKAGE_DEPENDS:-$default_depends}"
    for dependency in "${configured_depends[@]}"; do
        [[ -n "$dependency" ]] && depends+=(--depends "$dependency")
    done
    local fpm_arch="$architecture"
    [[ "$target" == "deb" && "$architecture" == "x86_64" ]] && fpm_arch="amd64"
    fpm -s dir -t "$target" \
        -n "$package_name" \
        -v "$version" \
        --force \
        --architecture "$fpm_arch" \
        --description "Local face authentication enrollment and diagnostics" \
        --license MIT \
        --maintainer "Smile2Unlock Project" \
        --vendor "Smile2Unlock" \
        --url "https://github.com/Smile2Unlock/Smile2Unlock_v2" \
        "${depends[@]}" \
        -C "$root_dir" \
        -p "${output_dir}/${package_name}-${version}.${target}" \
        usr
}

case "$format" in
    tar.gz) build_tarball ;;
    pacman) build_pacman ;;
    deb|rpm) build_fpm "$format" ;;
    all)
        build_tarball
        build_pacman
        build_fpm deb || true
        build_fpm rpm || true
        ;;
esac
