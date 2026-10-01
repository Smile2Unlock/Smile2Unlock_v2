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
unsigned_development=false
sign_certificate="${WINDOWS_SIGN_CERTIFICATE:-}"
sign_key="${WINDOWS_SIGN_KEY:-}"
timestamp_url="${WINDOWS_TIMESTAMP_URL:-}"

usage() {
    cat <<'USAGE'
Usage: packaging/windows/package.sh [options]

Options:
  --build-dir DIR       MinGW release build directory
  --output-dir DIR      Zip output directory (default: build/packages)
  --stage-only          Create the staging tree without a zip
  --no-zip              Alias for --stage-only
  --no-verify           Do not verify the completed zip
  --sign-certificate F  PEM signing certificate (or WINDOWS_SIGN_CERTIFICATE)
  --sign-key F          PEM private key (or WINDOWS_SIGN_KEY)
  --timestamp-url URL   RFC 3161 timestamp service (or WINDOWS_TIMESTAMP_URL)
  --unsigned-development
                        Build a non-deployable package for local inspection
  --help                Show this help
USAGE
}

while (($# > 0)); do
    case "$1" in
        --build-dir) [[ $# -ge 2 ]] || package_die "missing --build-dir value"; build_dir="$(package_absolute_path "$2")"; shift 2 ;;
        --output-dir) [[ $# -ge 2 ]] || package_die "missing --output-dir value"; output_dir="$(package_absolute_path "$2")"; shift 2 ;;
        --stage-only|--no-zip) stage_only=true; shift ;;
        --no-verify) verify=false; shift ;;
        --sign-certificate) [[ $# -ge 2 ]] || package_die "missing --sign-certificate value"; sign_certificate="$(package_absolute_path "$2")"; shift 2 ;;
        --sign-key) [[ $# -ge 2 ]] || package_die "missing --sign-key value"; sign_key="$(package_absolute_path "$2")"; shift 2 ;;
        --timestamp-url) [[ $# -ge 2 ]] || package_die "missing --timestamp-url value"; timestamp_url="$2"; shift 2 ;;
        --unsigned-development) unsigned_development=true; shift ;;
        --help|-h) usage; exit 0 ;;
        *) package_die "unknown argument: $1" ;;
    esac
done

build_dir="$(package_absolute_path "$build_dir")"
output_dir="$(package_absolute_path "$output_dir")"

if [[ "$unsigned_development" == true && "$stage_only" == false ]]; then
    package_die "--unsigned-development is allowed only with --stage-only"
fi

package_require_command x86_64-w64-mingw32-objdump
package_require_command python3

primary_files=(
    Smile2Unlock.exe
    Smile2UnlockDeployHelper.exe
    Smile2UnlockCredentialProvider.dll
    Smile2UnlockAuthService.exe
    Smile2UnlockPasswordTool.exe
    Smile2UnlockRecognitionAgent.exe
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
seeta_package_root="${SEETAFACE_PACKAGE_ROOT:-${xmake_home}/packages/s/seetaface6open}"

mingw_bin="${MINGW_BIN:-/usr/x86_64-w64-mingw32/bin}"
package_require_directory "$mingw_bin"

stage_root="${project_dir}/build/package-stage/windows"
package_root="${stage_root}/Smile2Unlock"
package_reset_stage "$stage_root"
mkdir -p "${package_root}/bin" "${package_root}/assets/i18n" \
    "${package_root}/assets/models/seeta"

install -m 0755 "${build_dir}/Smile2Unlock.exe" "${package_root}/bin/Smile2Unlock.exe"
install -m 0755 "${build_dir}/Smile2UnlockDeployHelper.exe" "${package_root}/bin/Smile2UnlockDeployHelper.exe"
install -m 0755 "${build_dir}/Smile2UnlockCredentialProvider.dll" "${package_root}/bin/Smile2UnlockCredentialProvider.dll"
install -m 0755 "${build_dir}/Smile2UnlockAuthService.exe" "${package_root}/bin/Smile2UnlockAuthService.exe"
install -m 0755 "${build_dir}/Smile2UnlockPasswordTool.exe" "${package_root}/bin/Smile2UnlockPasswordTool.exe"
install -m 0755 "${build_dir}/Smile2UnlockRecognitionAgent.exe" "${package_root}/bin/Smile2UnlockRecognitionAgent.exe"
install -m 0644 "${build_dir}/Smile2Unlock.ico" "${package_root}/bin/Smile2Unlock.ico"
install -m 0644 "${build_dir}/assets/i18n/"*.json "${package_root}/assets/i18n/"
install -m 0644 "${build_dir}/assets/models/seeta/"*.csta "${package_root}/assets/models/seeta/"

# Third-party attribution: project license, notices, and every license text
# (Slint triple-license, SeetaFace6 BSD-2, libjpeg-turbo IJG notice, Rust
# crates inventory) ship next to the binaries they cover.
install -m 0644 "${project_dir}/LICENSE" "${package_root}/LICENSE"
install -m 0644 "${project_dir}/NOTICE/THIRD-PARTY-NOTICES.md" "${package_root}/"
cp -r "${project_dir}/licenses" "${package_root}/licenses"

find_runtime_dll() {
    local name="$1"
    local candidate
    # A build can stage its exact SDK DLLs. Prefer these over another cache
    # entry (e.g. the pre-Unicode package) with the same runtime filename.
    candidate="$(find "$build_dir" -maxdepth 1 -type f -iname "$name" -print -quit)"
    if [[ -z "$candidate" ]]; then
        candidate="$(find "$mingw_bin" -maxdepth 1 -type f -iname "$name" -print -quit)"
    fi
    if [[ -z "$candidate" ]]; then
        local candidates=()
        # Only installed x64 runtimes are eligible. SDK source/build trees
        # also contain copies, even with a single installed package.
        if [[ -d "$seeta_package_root" ]]; then
            mapfile -t candidates < <(find "$seeta_package_root" -type d -name src -prune -o \
                -type f \( -path '*/bin/x64/*' -o -path '*/lib/x64/*' \) -iname "$name" -print)
        fi
        if ((${#candidates[@]} > 1)); then
            package_die "ambiguous SDK DLL $name; stage the exact build DLLs or set SEETAFACE_PACKAGE_ROOT"
        fi
        candidate="${candidates[0]:-}"
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

for binary in "${package_root}/bin/"*.exe "${package_root}/bin/Smile2UnlockCredentialProvider.dll"; do
    stage_dependencies "$binary"
done

# TenniS loads these at runtime on CPUs that cannot use its baseline build;
# they do not appear in the PE import table scanned above.
for name in libtennis_haswell.dll libtennis_sandy_bridge.dll libtennis_pentium.dll; do
    source="$(find_runtime_dll "$name")"
    [[ -n "$source" ]] || package_die "CPU runtime DLL was not found: $name"
    install -m 0755 "$source" "${package_root}/bin/${name}"
    stage_dependencies "$source"
done

if [[ "$unsigned_development" == false ]]; then
    package_require_file "$sign_certificate"
    package_require_file "$sign_key"
    package_require_command osslsigncode
    package_require_command openssl
    sign_args=(-certs "$sign_certificate" -key "$sign_key" -h sha256 -n "Smile2Unlock")
    if [[ -n "$timestamp_url" ]]; then
        sign_args+=(-ts "$timestamp_url")
    fi
    while IFS= read -r -d '' binary; do
        signed="${binary}.signed"
        osslsigncode sign "${sign_args[@]}" -in "$binary" -out "$signed" >/dev/null
        mv -- "$signed" "$binary"
    done < <(find "${package_root}/bin" -maxdepth 1 -type f \
        \( -iname '*.exe' -o -iname '*.dll' \) -print0)
else
    : > "${package_root}/UNSIGNED-DEVELOPMENT-PACKAGE"
fi

PACKAGE_ARCH="$architecture" inject_release_info "$package_root" windows
if [[ "$unsigned_development" == false ]]; then
    openssl cms -sign -binary -in "${package_root}/release-info.json" \
        -signer "$sign_certificate" -inkey "$sign_key" -outform DER \
        -out "${package_root}/release-info.p7s" -nosmimecap
fi
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

# NSIS setup exe from the SAME staged tree: the installer embeds this package
# verbatim and only orchestrates the audited su_deploy_helper, so ZIP,
# installer and release-info.json cannot drift apart. Built whenever makensis
# is installed; signing reuses the certificate configured above.
if command -v makensis >/dev/null 2>&1; then
    setup="${output_dir}/${package_name}-${package_version}-windows-${architecture}-setup.exe"
    makensis -V2 \
        -DVERSION="${package_version}" \
        -DSTAGE_DIR="${stage_root}" \
        -DOUT_FILE="${setup}" \
        "${script_dir}/installer.nsi"
    if [[ "$unsigned_development" == false ]]; then
        signed_setup="${setup}.signed"
        osslsigncode sign "${sign_args[@]}" -in "$setup" -out "$signed_setup" >/dev/null
        mv -- "$signed_setup" "$setup"
        authenticode_verify_args=()
        if [[ -n "${WINDOWS_VERIFY_CA_FILE:-}" ]]; then
            authenticode_verify_args=(-CAfile "$WINDOWS_VERIFY_CA_FILE")
        fi
        osslsigncode verify "${authenticode_verify_args[@]}" -in "$setup" >/dev/null \
            || package_die "invalid Authenticode signature: $(basename "$setup")"
    fi
    echo "created ${setup}"
else
    echo "note: makensis not found, skipped the NSIS setup exe"
fi
