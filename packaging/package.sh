#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${script_dir}/lib/common.sh"

platform="all"
linux_format="tar.gz"
output_dir="${project_dir}/build/packages"
build=false
jobs="${JOBS:-$(nproc)}"

usage() {
    cat <<'USAGE'
Usage: packaging/package.sh [options]

Build and package every supported desktop platform from one entry point.

Options:
  --platform PLATFORM   all, linux, or windows (default: all)
  --build               Configure and rebuild release targets before packaging
  --linux-format FORMAT tar.gz, pacman, deb, rpm, or all (default: tar.gz)
  --output-dir DIR      Package output directory (default: build/packages)
  --jobs COUNT          Parallel build jobs (default: host CPU count)
  --help                Show this help

Without --build, existing release artifacts under build/<platform>/x86_64/release
are packaged and verified. The default all-platform output is a Linux tarball
and a Windows zip.
USAGE
}

while (($# > 0)); do
    case "$1" in
        --platform) [[ $# -ge 2 ]] || package_die "missing --platform value"; platform="$2"; shift 2 ;;
        --build) build=true; shift ;;
        --linux-format) [[ $# -ge 2 ]] || package_die "missing --linux-format value"; linux_format="$2"; shift 2 ;;
        --output-dir) [[ $# -ge 2 ]] || package_die "missing --output-dir value"; output_dir="$(package_absolute_path "$2")"; shift 2 ;;
        --jobs) [[ $# -ge 2 ]] || package_die "missing --jobs value"; jobs="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) package_die "unknown argument: $1" ;;
    esac
done

case "$platform" in all|linux|windows) ;; *) package_die "unsupported platform: $platform" ;; esac
case "$linux_format" in tar.gz|pacman|deb|rpm|all) ;; *) package_die "unsupported Linux format: $linux_format" ;; esac
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || package_die "--jobs must be a positive integer"

package_require_command xmake
mkdir -p "$output_dir"

build_targets() {
    local target_platform="$1"
    shift
    echo "==> configuring ${target_platform} release build"
    xmake f -p "$target_platform" -a x86_64 -m release --with_slint=y --with_seetaface=y

    # C++ module BMIs are compiler-specific. Cleaning the selected target graph
    # prevents a GCC/MinGW switch from reusing incompatible module state.
    local target
    for target in "$@"; do
        xmake clean "$target"
    done
    echo "==> building ${target_platform} release targets"
    xmake -r -j "$jobs" "$@"
}

run_linux() {
    local args=(--format "$linux_format" --output-dir "$output_dir")
    if [[ "$build" == true ]]; then
        build_targets linux su_app su_authd su_deploy_helper pam_smile2unlock
    fi
    "${script_dir}/linux/package.sh" "${args[@]}"
}

run_windows() {
    local args=(--output-dir "$output_dir")
    if [[ "$build" == true ]]; then
        build_targets mingw su_app su_auth_service su_deploy_helper \
            su_password_tool su_recognition_agent su_credential_provider
    fi
    "${script_dir}/windows/package.sh" "${args[@]}"
}

case "$platform" in
    linux) run_linux ;;
    windows) run_windows ;;
    all)
        run_linux
        run_windows
        ;;
esac

echo "==> packages written to ${output_dir}"
