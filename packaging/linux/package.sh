#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/common.sh
source "${script_dir}/../lib/common.sh"
# shellcheck source=../version/versions.sh
source "${script_dir}/../version/versions.sh"

build_dir="${BUILD_DIR:-${project_dir}/build/linux/x86_64/release}"
output_dir="${OUTPUT_DIR:-${project_dir}/build/packages}"
format="${PACKAGE_FORMAT:-tar.gz}"
pam_module_dir="${PAM_MODULE_DIR:-/usr/lib/security}"
architecture="${PACKAGE_ARCH:-x86_64}"
stage_only=false
verify=true

usage() {
    cat <<'USAGE'
Usage: packaging/linux/package.sh [options]

Options:
  --format FORMAT       tar.gz, pacman, deb, rpm, or all (default: tar.gz)
  --build-dir DIR       Linux release build directory
  --output-dir DIR      Package output directory (default: build/packages)
  --pam-module-dir DIR  Absolute PAM module directory (default: /usr/lib/security)
  --arch ARCH           Package architecture (default: x86_64)
  --stage-only          Create the common filesystem tree without an archive
  --no-verify           Do not verify completed packages
  --help                Show this help

Environment:
  PACKAGE_DEPENDS       Override comma-separated dependencies for DEB/RPM
USAGE
}

while (($# > 0)); do
    case "$1" in
        --format) [[ $# -ge 2 ]] || package_die "missing --format value"; format="$2"; shift 2 ;;
        --build-dir) [[ $# -ge 2 ]] || package_die "missing --build-dir value"; build_dir="$(package_absolute_path "$2")"; shift 2 ;;
        --output-dir) [[ $# -ge 2 ]] || package_die "missing --output-dir value"; output_dir="$(package_absolute_path "$2")"; shift 2 ;;
        --pam-module-dir) [[ $# -ge 2 ]] || package_die "missing --pam-module-dir value"; pam_module_dir="$2"; shift 2 ;;
        --arch) [[ $# -ge 2 ]] || package_die "missing --arch value"; architecture="$2"; shift 2 ;;
        --stage-only) stage_only=true; shift ;;
        --no-verify) verify=false; shift ;;
        --help|-h) usage; exit 0 ;;
        *) package_die "unknown argument: $1" ;;
    esac
done
build_dir="$(package_absolute_path "$build_dir")"
output_dir="$(package_absolute_path "$output_dir")"
case "$format" in tar.gz|pacman|deb|rpm|all) ;; *) package_die "unsupported format: $format" ;; esac
[[ "$pam_module_dir" == /* ]] || package_die "PAM module directory must be absolute"
[[ "$architecture" =~ ^[A-Za-z0-9_.-]+$ ]] || package_die "invalid architecture: $architecture"

package_require_command patchelf
package_require_command python3
for name in su_app su_authd su_deploy_helper pam_smile2unlock.so; do
    package_require_file "${build_dir}/${name}"
done
for name in en.json zh-CN.json; do
    package_require_file "${build_dir}/assets/i18n/${name}"
done
for name in face_detector.csta face_landmarker_pts5.csta face_recognizer.csta \
    fas_first.csta fas_second.csta; do
    package_require_file "${build_dir}/assets/models/seeta/${name}"
done

case "$format" in
    pacman) package_require_command makepkg ;;
    deb|rpm) package_require_command fpm ;;
    all)
        package_require_command makepkg
        package_require_command fpm
        ;;
esac

stage_root="${project_dir}/build/package-stage/linux"
root_dir="${stage_root}/root"
package_reset_stage "$stage_root"
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
    "$output_dir"

install -m 0755 "${build_dir}/su_app" "${root_dir}/usr/bin/su_app"
install -m 0755 "${build_dir}/su_authd" "${root_dir}/usr/libexec/smile2unlock/su_authd"
install -m 0755 "${build_dir}/su_deploy_helper" "${root_dir}/usr/libexec/smile2unlock/su_deploy_helper"
install -m 0755 "${project_dir}/packaging/install-dms-lock.sh" "${root_dir}/usr/libexec/smile2unlock/install-dms-lock"
install -m 0755 "${project_dir}/packaging/setup-storage-key.sh" "${root_dir}/usr/libexec/smile2unlock/setup-storage-key"
install -m 0755 "${build_dir}/pam_smile2unlock.so" "${root_dir}${pam_module_dir}/pam_smile2unlock.so"

install -m 0644 "${project_dir}/packaging/systemd/su-authd.service" "${root_dir}/usr/lib/systemd/system/"
install -m 0644 "${project_dir}/packaging/systemd/su-deploy-helper.service" "${root_dir}/usr/lib/systemd/system/"
install -m 0644 "${project_dir}/packaging/dbus/io.github.smile2unlock.Deployment1.service" \
    "${root_dir}/usr/share/dbus-1/system-services/"
install -m 0644 "${project_dir}/packaging/dbus/io.github.smile2unlock.Deployment1.conf" \
    "${root_dir}/usr/share/dbus-1/system.d/"
install -m 0644 "${project_dir}/packaging/polkit/io.github.smile2unlock.deployment.policy" \
    "${root_dir}/usr/share/polkit-1/actions/"
install -m 0644 "${project_dir}/packaging/linux/smile2unlock.desktop" \
    "${root_dir}/usr/share/applications/"
install -m 0644 "${project_dir}/assets/icons/Smile2Unlock.png" \
    "${root_dir}/usr/share/icons/hicolor/128x128/apps/smile2unlock.png"
install -m 0644 "${project_dir}/packaging/linux/pam/dankshell-smile2unlock" \
    "${root_dir}/usr/share/smile2unlock/pam/"
install -m 0644 "${build_dir}/assets/i18n/"*.json "${root_dir}/usr/share/smile2unlock/i18n/"
install -m 0644 "${build_dir}/assets/models/seeta/"*.csta "${root_dir}/usr/share/smile2unlock/models/"
install -m 0644 "${project_dir}/LICENSE" "${root_dir}/usr/share/doc/smile2unlock/LICENSE"
install -m 0644 "${project_dir}/NOTICE/THIRD-PARTY-NOTICES.md" "${root_dir}/usr/share/doc/smile2unlock/"
install -m 0644 "${project_dir}/packaging/linux/README.md" "${root_dir}/usr/share/doc/smile2unlock/README.md"
install -m 0644 "${project_dir}/docs/linux_pam_setup.md" "${root_dir}/usr/share/doc/smile2unlock/"

app_rpath="$(patchelf --print-rpath "${build_dir}/su_app")"
slint_dir=""
seeta_dir=""
IFS=':' read -r -a runtime_dirs <<< "$app_rpath"
for directory in "${runtime_dirs[@]}"; do
    [[ -n "$slint_dir" || ! -f "${directory}/libslint_cpp.so" ]] || slint_dir="$directory"
    [[ -n "$seeta_dir" || ! -f "${directory}/libSeetaFaceDetector600.so" ]] || seeta_dir="$directory"
done
[[ -n "$slint_dir" ]] || package_die "cannot locate the Slint runtime from su_app RPATH"
[[ -n "$seeta_dir" ]] || package_die "cannot locate the SeetaFace runtime from su_app RPATH"

shopt -s nullglob
runtime_libraries=(
    "${slint_dir}/libslint_cpp.so"*
    "${seeta_dir}/libSeeta"*.so*
    "${seeta_dir}/libtennis"*.so*
)
shopt -u nullglob
(( ${#runtime_libraries[@]} > 0 )) || package_die "no private runtime libraries found"
cp -a "${runtime_libraries[@]}" "${root_dir}/usr/lib/smile2unlock/"
find "${root_dir}/usr/lib/smile2unlock" -type f -name '*.so*' -exec chmod 0755 {} +
while IFS= read -r -d '' library; do
    patchelf --set-rpath '$ORIGIN' "$library"
done < <(find "${root_dir}/usr/lib/smile2unlock" -type f -name '*.so*' -print0)
patchelf --set-rpath '$ORIGIN/../lib/smile2unlock' "${root_dir}/usr/bin/su_app"
patchelf --set-rpath '$ORIGIN/../../lib/smile2unlock' "${root_dir}/usr/libexec/smile2unlock/su_authd"

PACKAGE_ARCH="$architecture" inject_release_info "$root_dir" linux \
    "${root_dir}/usr/share/smile2unlock/release-info.json"
echo "staged ${root_dir} ($(package_file_count "$root_dir") files)"

if [[ "$stage_only" == true ]]; then
    exit 0
fi

declare -a produced=()

build_tarball() {
    package_require_command tar
    local tree="${stage_root}/${package_name}-${package_version}"
    cp -a "$root_dir" "$tree"
    local archive="${output_dir}/${package_name}-${package_version}-linux-${architecture}.tar.gz"
    rm -f -- "$archive"
    tar -C "$stage_root" --owner=0 --group=0 -czf "$archive" "$(basename "$tree")"
    produced+=("$archive")
}

build_pacman() {
    local pkgbuild="${stage_root}/PKGBUILD"
    local install_script="${stage_root}/smile2unlock.install"
    cat > "$install_script" <<EOF
pre_upgrade() {
    if [[ -x /usr/libexec/smile2unlock/su_deploy_helper ]]; then
        /usr/libexec/smile2unlock/su_deploy_helper --check-package-version '${package_version}'
    fi
}
pre_remove() {
    /usr/libexec/smile2unlock/su_deploy_helper --rollback-all
    systemctl disable --now su-authd.service >/dev/null 2>&1 || true
}
EOF
    cat > "$pkgbuild" <<EOF
pkgname=${package_name}
pkgver=${package_version}
pkgrel=1
pkgdesc='Local face authentication enrollment and diagnostics'
arch=('${architecture}')
license=('MIT')
options=('!debug' '!strip')
install=smile2unlock.install
depends=('pam' 'systemd-libs' 'dbus' 'polkit' 'gcc-libs')
_stage_root='${root_dir}'

package() {
    install -d "\$pkgdir"
    cp -a --no-preserve=ownership "\$_stage_root"/. "\$pkgdir"/
}
EOF
    local makepkg_config="${stage_root}/makepkg.conf"
    {
        echo 'source /etc/makepkg.conf'
        printf 'PKGDEST=%q\n' "$output_dir"
    } > "$makepkg_config"
    (cd "$stage_root" && makepkg --force --nodeps --config "$makepkg_config" -p PKGBUILD)
    local archive="${output_dir}/${package_name}-${package_version}-1-${architecture}.pkg.tar.zst"
    package_require_file "$archive"
    produced+=("$archive")
}

build_fpm() {
    local target="$1"
    local default_depends
    case "$target" in
        deb) default_depends="libc6,libstdc++6,libpam0g,libgomp1,libsystemd0,dbus,polkitd" ;;
        rpm) default_depends="glibc,libstdc++,pam,libgomp,systemd-libs,dbus,polkit" ;;
    esac
    local -a dependency_args=()
    local dependency
    IFS=',' read -r -a configured_depends <<< "${PACKAGE_DEPENDS:-$default_depends}"
    for dependency in "${configured_depends[@]}"; do
        [[ -z "$dependency" ]] || dependency_args+=(--depends "$dependency")
    done
    local fpm_arch="$architecture"
    [[ "$target" != deb || "$architecture" != x86_64 ]] || fpm_arch=amd64
    local archive="${output_dir}/${package_name}-${package_version}-linux-${architecture}.${target}"
    local pre_install="${stage_root}/pre-install-${target}.sh"
    local pre_remove="${stage_root}/pre-remove-${target}.sh"
    sed "s/@PACKAGE_VERSION@/${package_version}/g" \
        "${script_dir}/maintainer/pre-install.sh" > "$pre_install"
    cp "${script_dir}/maintainer/pre-remove.sh" "$pre_remove"
    chmod 0755 "$pre_install" "$pre_remove"
    fpm -s dir -t "$target" -n "$package_name" -v "$package_version" --force \
        --architecture "$fpm_arch" \
        --description "Local face authentication enrollment and diagnostics" \
        --license MIT --maintainer "Smile2Unlock Project" --vendor Smile2Unlock \
        --url "https://github.com/Smile2Unlock/Smile2Unlock_v2" \
        --before-install "$pre_install" --before-remove "$pre_remove" \
        "${dependency_args[@]}" -C "$root_dir" -p "$archive" usr
    package_require_file "$archive"
    produced+=("$archive")
}

case "$format" in
    tar.gz) build_tarball ;;
    pacman) build_pacman ;;
    deb|rpm) build_fpm "$format" ;;
    all)
        build_tarball
        build_pacman
        build_fpm deb
        build_fpm rpm
        ;;
esac

if [[ "$verify" == true ]]; then
    "${script_dir}/verify-package.sh" "${produced[@]}"
fi
for archive in "${produced[@]}"; do
    echo "created ${archive}"
done
