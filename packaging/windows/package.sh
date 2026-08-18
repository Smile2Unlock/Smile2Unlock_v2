#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/common.sh
source "${script_dir}/../lib/common.sh"
# shellcheck source=../version/versions.sh
source "${script_dir}/../version/versions.sh"
# shellcheck source=system-dlls.sh
source "${script_dir}/system-dlls.sh"

build_dir="${BUILD_DIR:-${project_dir}/build/mingw/x86_64/release}"
output_dir="${OUTPUT_DIR:-${project_dir}/build/packages}"
architecture="x86_64"
stage_only=false
verify=true

usage() {
    cat <<'USAGE'
Usage: packaging/windows/package.sh [options]

Options:
  --build-dir DIR       MinGW release build directory
  --output-dir DIR      Zip output directory (default: build/packages)
  --stage-only          Create the staging tree without a zip
  --no-zip              Alias for --stage-only
  --no-verify           Do not verify the completed zip
  --help                Show this help
USAGE
}

while (($# > 0)); do
    case "$1" in
        --build-dir) [[ $# -ge 2 ]] || package_die "missing --build-dir value"; build_dir="$(package_absolute_path "$2")"; shift 2 ;;
        --output-dir) [[ $# -ge 2 ]] || package_die "missing --output-dir value"; output_dir="$(package_absolute_path "$2")"; shift 2 ;;
        --stage-only|--no-zip) stage_only=true; shift ;;
        --no-verify) verify=false; shift ;;
        --help|-h) usage; exit 0 ;;
        *) package_die "unknown argument: $1" ;;
    esac
done

build_dir="$(package_absolute_path "$build_dir")"
output_dir="$(package_absolute_path "$output_dir")"

package_require_command x86_64-w64-mingw32-objdump
package_require_command python3

primary_files=(
    su_app.exe
    su_deploy_helper.exe
    su_credential_provider.dll
    su_auth_service.exe
    su_password_tool.exe
    su_recognition_agent.exe
    Smile2Unlock.ico
)
for name in "${primary_files[@]}"; do
    package_require_file "${build_dir}/${name}"
done
for name in en.json zh-CN.json; do
    package_require_file "${build_dir}/assets/i18n/${name}"
done
for name in face_detector.csta face_landmarker_pts5.csta face_recognizer.csta \
    fas_first.csta fas_second.csta; do
    package_require_file "${build_dir}/assets/models/seeta/${name}"
done

xmake_home="${XMAKE_GLOBALDIR:-${HOME}/.xmake}"
seeta_package_root="${xmake_home}/packages/s/seetaface6open"
find "$seeta_package_root" -type f -iname 'libSeetaFaceDetector600.dll' -print -quit \
    | grep -q . || package_die "cannot locate the MinGW SeetaFace package"

mingw_bin="${MINGW_BIN:-/usr/x86_64-w64-mingw32/bin}"
package_require_directory "$mingw_bin"

stage_root="${project_dir}/build/package-stage/windows"
package_root="${stage_root}/Smile2Unlock"
package_reset_stage "$stage_root"
mkdir -p "${package_root}/bin" "${package_root}/assets/i18n" \
    "${package_root}/assets/models/seeta"

install -m 0755 "${build_dir}/su_app.exe" "${package_root}/bin/su_app.exe"
install -m 0755 "${build_dir}/su_deploy_helper.exe" "${package_root}/bin/su_deploy_helper.exe"
install -m 0755 "${build_dir}/su_credential_provider.dll" "${package_root}/bin/su_credential_provider.dll"
install -m 0755 "${build_dir}/su_auth_service.exe" "${package_root}/bin/Smile2UnlockAuthService.exe"
install -m 0755 "${build_dir}/su_password_tool.exe" "${package_root}/bin/su_password_tool.exe"
install -m 0755 "${build_dir}/su_recognition_agent.exe" "${package_root}/bin/su_recognition_agent.exe"
install -m 0644 "${build_dir}/Smile2Unlock.ico" "${package_root}/bin/Smile2Unlock.ico"
install -m 0644 "${build_dir}/assets/i18n/"*.json "${package_root}/assets/i18n/"
install -m 0644 "${build_dir}/assets/models/seeta/"*.csta "${package_root}/assets/models/seeta/"

find_runtime_dll() {
    local name="$1"
    local candidate
    candidate="$(find "$mingw_bin" -maxdepth 1 -type f -iname "$name" -print -quit)"
    if [[ -z "$candidate" ]]; then
        candidate="$(find "$seeta_package_root" -type f -iname "$name" -print -quit)"
    fi
    printf '%s\n' "$candidate"
}

declare -A collected=()
stage_dependencies() {
    local binary="$1"
    local name key source
    while IFS= read -r name; do
        [[ -n "$name" ]] || continue
        key="${name,,}"
        [[ -z "${collected[$key]:-}" ]] || continue
        collected[$key]=1
        windows_is_system_dll "$name" && continue
        source="$(find_runtime_dll "$name")"
        if [[ -z "$source" ]]; then
            [[ -f "${package_root}/bin/${name}" ]] && continue
            package_die "runtime DLL required by $(basename "$binary") was not found: $name"
        fi
        install -m 0755 "$source" "${package_root}/bin/$(basename "$source")"
        stage_dependencies "$source"
    done < <(x86_64-w64-mingw32-objdump -p "$binary" 2>/dev/null \
        | sed -n 's/^[[:space:]]*DLL Name: \(.*\)/\1/p')
}

for binary in "${package_root}/bin/"*.exe "${package_root}/bin/su_credential_provider.dll"; do
    stage_dependencies "$binary"
done

PACKAGE_ARCH="$architecture" inject_release_info "$package_root" windows
echo "staged ${package_root} ($(package_file_count "$package_root") files)"

if [[ "$stage_only" == true ]]; then
    exit 0
fi
package_require_command zip
mkdir -p "$output_dir"
archive="${output_dir}/${package_name}-${package_version}-windows-${architecture}.zip"
rm -f -- "$archive"
(cd "$stage_root" && zip -q -r "$archive" Smile2Unlock)

if [[ "$verify" == true ]]; then
    "${script_dir}/verify-package.sh" "$archive"
fi
echo "created ${archive}"
