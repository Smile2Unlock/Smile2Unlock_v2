#!/usr/bin/env bash
# Windows deployment package builder.
#
# A single command that produces a ready-to-deploy directory + zip for the
# Windows build, mirroring packaging/linux/package.sh:
#
#   packaging/windows/package.sh [--build-dir DIR] [--output-dir DIR] [--no-zip]
#
# Stages into:
#   build/windows-package/smile2unlock-<version>/
#     bin\    su_app.exe, su_deploy_helper.exe, su_credential_provider.dll,
#             Smile2UnlockAuthService.exe, Smile2Unlock.ico, runtime DLLs
#     assets\ i18n\ + models\seeta\ (same tree the GUI resolves at runtime)
#
# and writes build/packages/smile2unlock-<version>-windows-x86_64.zip.
#
# Runtime DLL collection: every binary's NEEDED table is scanned recursively
# (mingw objdump). Non-system DLLs are copied:
#   - MinGW runtime (libgcc_s_seh-1.dll, libstdc++-6.dll, libwinpthread-1.dll,
#     libgomp-1.dll) from the cross toolchain
#   - SeetaFace + tennis from the local xmake package installation tree
# System DLLs (kernel32/ntdll/combase/ole32/... and api-ms-*) are always left
# to the OS — Win10+ ships them.

set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${BUILD_DIR:-${project_dir}/build/mingw/x86_64/release}"
output_dir="${OUTPUT_DIR:-${project_dir}/build/packages}"
package_name="smile2unlock"
version="$(tr -d '[:space:]' < "${project_dir}/version.txt")"
# Version / release manifest helpers (release-info + releases.json).
# shellcheck source=../version/versions.sh
source "${project_dir}/packaging/version/versions.sh"
version="$(version_read)"
arch="x86_64"
work_root="${project_dir}/build/windows-package"
package_root="${work_root}/${package_name}-${version}"

usage() {
    cat <<'USAGE'
Usage: packaging/windows/package.sh [options]

Options:
  --build-dir DIR       Release build directory (default: build/mingw/x86_64/release)
  --output-dir DIR      Zip output directory (default: build/packages)
  --no-zip              Stage the directory but do not create the zip
  --help                Show this help

Environment:
  BUILD_DIR / OUTPUT_DIR   Override the corresponding options
USAGE
}

no_zip=false
while (($# > 0)); do
    case "$1" in
        --build-dir) [[ $# -ge 2 ]] || { echo "missing --build-dir value" >&2; exit 64; }; build_dir="$2"; shift 2 ;;
        --output-dir) [[ $# -ge 2 ]] || { echo "missing --output-dir value" >&2; exit 64; }; output_dir="$2"; shift 2 ;;
        --no-zip) no_zip=true; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 64 ;;
    esac
done

require_file() {
    [[ -f "$1" ]] || { echo "missing build artifact: $1" >&2; exit 1; }
}

require_file "${build_dir}/su_app.exe"
require_file "${build_dir}/su_deploy_helper.exe"
require_file "${build_dir}/su_credential_provider.dll"
require_file "${build_dir}/su_auth_service.exe"
require_file "${build_dir}/assets/i18n/en.json"
require_file "${build_dir}/assets/i18n/zh-CN.json"
for model in face_detector.csta face_landmarker_pts5.csta face_recognizer.csta fas_first.csta fas_second.csta; do
    require_file "${build_dir}/assets/models/seeta/${model}"
done

command -v x86_64-w64-mingw32-objdump >/dev/null || {
    echo "x86_64-w64-mingw32-objdump is required (mingw-w64 toolchain)" >&2
    exit 1
}

# Locate the installed SeetaFace mingw package: the install dir that contains
# the built DLLs (the Linux package install has lib64/*.so but no DLLs).
seeta_pkg=""
for dir in "${HOME}/.xmake/packages/s/seetaface6open/latest/"*/; do
    if [[ -d "$dir" ]] && find "$dir" -type f -name "libSeetaFaceDetector600.dll" 2>/dev/null | grep -q .; then
        seeta_pkg="${dir%/}"
        break
    fi
done
[[ -n "${seeta_pkg}" ]] || {
    echo "cannot locate the seetaface6open mingw package under ~/.xmake/packages/s/seetaface6open" >&2
    exit 1
}
mingw_bin="${MINGW_BIN:-/usr/x86_64-w64-mingw32/bin}"
[[ -d "${mingw_bin}" ]] || {
    echo "cannot locate MinGW runtime DLL dir (set MINGW_BIN)" >&2
    exit 1
}

# is_system_dll NAME: returns 0 if the DLL is provided by the OS (skip) or
# 1 if it must be bundled (collect). Lowercases the name first.
is_system_dll() {
    local name="${1,,}"
    case "$name" in
        kernel32.dll|ntdll.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll) return 0 ;;
        combase.dll|ole32.dll|oleaut32.dll|rpcrt4.dll|shlwapi.dll|ws2_32.dll|winmm.dll) return 0 ;;
        dwmapi.dll|dwrite.dll|imm32.dll|comctl32.dll|uxtheme.dll|msvcrt.dll|ucrtbase.dll) return 0 ;;
        mfplat.dll|mfreadwrite.dll|mfuuid.dll|mf.dll|mfperfhelper.dll|evr.dll|dxva2.dll) return 0 ;;
        credui.dll|secur32.dll|crypt32.dll|bcryptprimitives.dll|ncrypt.dll|userenv.dll) return 0 ;;
        version.dll|d3d11.dll|d3d9.dll|dxgi.dll|opengl32.dll|opengl32sw.dll|gdiplus.dll) return 0 ;;
        sspicli.dll|winhttp.dll|dhcpcsvc.dll|dhcpcsvc6.dll|iphlpapi.dll|setupapi.dll) return 0 ;;
        cfgmgr32.dll|powrprof.dll|wtsapi32.dll|psapi.dll|kernelbase.dll|windows.storage.dll) return 0 ;;
        propsys.dll|clbcatq.dll|onecoreuap.dll|wldp.dll|bcrypt.dll|cryptbase.dll) return 0 ;;
        api-ms-*) return 0 ;;
        *) return 1 ;;
    esac
}

# find_dll NAME -> absolute path of a bundled runtime DLL, or empty.
find_dll() {
    local name="$1"
    if [[ -f "${mingw_bin}/${name}" ]]; then
        echo "${mingw_bin}/${name}"
        return 0
    fi
    local found
    found="$(find "${seeta_pkg}" -type f -name "${name}" 2>/dev/null | head -1)"
    echo "${found:-}"
}

rm -rf "${package_root}"
mkdir -p "${package_root}/bin" "${package_root}/assets"

# --- 1. Copy the primary binaries -------------------------------------------
install -m 0755 "${build_dir}/su_app.exe" "${package_root}/bin/su_app.exe"
install -m 0755 "${build_dir}/su_deploy_helper.exe" "${package_root}/bin/su_deploy_helper.exe"
install -m 0755 "${build_dir}/su_credential_provider.dll" "${package_root}/bin/su_credential_provider.dll"
install -m 0755 "${build_dir}/su_auth_service.exe" "${package_root}/bin/Smile2UnlockAuthService.exe"
install -m 0644 "${build_dir}/Smile2Unlock.ico" "${package_root}/bin/Smile2Unlock.ico"

# --- 2. Copy runtime DLLs recursively from NEEDED tables ---------------------
declare -A collected=()
stage_deps() {
    local binary="$1"
    local name
    while IFS= read -r name; do
        [[ -z "$name" ]] && continue
        [[ -n "${collected[$name]:-}" ]] && continue
        if is_system_dll "$name"; then
            continue  # provided by Windows, skip
        fi
        collected[$name]=1
        local path
        path="$(find_dll "$name")"
        if [[ -z "$path" ]]; then
            # Already staged next to the binary (e.g. su_credential_provider.dll)
            if [[ -f "${package_root}/bin/${name}" ]]; then
                continue
            fi
            echo "WARNING: runtime DLL not found for ${binary}: ${name}" >&2
            continue
        fi
        install -m 0755 "$path" "${package_root}/bin/${name}"
        stage_deps "$path"
    done < <(x86_64-w64-mingw32-objdump -p "$binary" 2>/dev/null | sed -n 's/^\s*DLL Name: \(.*\)/\1/p')
}

for binary in \
    "${package_root}/bin/su_app.exe" \
    "${package_root}/bin/su_deploy_helper.exe" \
    "${package_root}/bin/Smile2UnlockAuthService.exe" \
    "${package_root}/bin/su_credential_provider.dll"
do
    stage_deps "$binary"
done

# --- 3. Stage assets --------------------------------------------------------
cp -a "${build_dir}/assets/i18n" "${package_root}/assets/i18n"
mkdir -p "${package_root}/assets/models"
cp -a "${build_dir}/assets/models/seeta" "${package_root}/assets/models/seeta"

echo "staged: ${package_root}"
echo "  bin: $(find "${package_root}/bin" -maxdepth 1 -type f | wc -l) files"
echo "  assets: $(find "${package_root}/assets" -type f | wc -l) files"

# --- 3.5 Inject release-info.json (version/stream + per-file SHA256) --------
inject_release_info "${package_root}" "windows"

# --- 4. Zip -----------------------------------------------------------------
if [[ "$no_zip" == true ]]; then
    echo "skipped zip"
    exit 0
fi
command -v zip >/dev/null || {
    echo "zip is required (or re-run with --no-zip)" >&2
    exit 1
}
mkdir -p "${output_dir}"
zip_file="${output_dir}/${package_name}-${version}-windows-${arch}.zip"
rm -f "${zip_file}"
(cd "${work_root}" && zip -qr "${zip_file}" "${package_name}-${version}")
manifest_add "Smile2Unlock ${version} (Windows, zip)" "${zip_file}"
echo "created ${zip_file}"
