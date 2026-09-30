#!/usr/bin/env bash
# Produce a reusable prebuilt mingw-x86_64 Slint C++ package for CI.
#
# The Windows release job builds Slint from source twice (host slint-compiler
# + windows-gnu runtime) which costs ~15 minutes every cold cache. This script
# packages the installed xmake package directory (headers, static lib, host
# compiler, licenses) so CI can drop it straight into ~/.xmake/packages and
# skip the source build entirely.
#
#   packaging/slint/package-slint-mingw.sh
#
# Output: build/packages/slint-mingw-x86_64-v1.17.0.tar.gz
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version="v1.17.0"
installdir="${HOME}/.xmake/packages/s/slint/${version}"
[[ -d "${installdir}" ]] || {
    echo "slint: installed mingw package not found under ${installdir};" >&2
    echo "build it first: xmake f -p mingw ... && xmake build su_app" >&2
    exit 1
}
# The install dir contains hash subdirs (one per build config); the mingw
# variant is the one carrying the windows-gnu static library.
pkg_dir=""
for dir in "${installdir}"/*/; do
    if [[ -f "${dir}lib/libslint_cpp.a" && -d "${dir}bin" ]]; then
        pkg_dir="${dir%/}"
    fi
done
[[ -n "${pkg_dir}" ]] || { echo "slint: no built mingw variant found" >&2; exit 1; }

out="${project_dir}/build/packages"
mkdir -p -- "${out}"
archive="${out}/slint-mingw-x86_64-${version}.tar.gz"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp}"' EXIT
# Preserve the source directory's hash name: xmake resolves the package to
# the directory whose hash matches recipe+config, and our mingw config is
# what CI reproduces, so the same name makes the seed resolve cleanly.
hash_name="$(basename "${pkg_dir}")"
mkdir -p "${tmp}/slint/${version}/${hash_name}"
cp -r "${pkg_dir}/." "${tmp}/slint/${version}/${hash_name}/"
( cd "${tmp}" && tar -czf "${archive}" slint )
sha256sum "${archive}" > "${archive}.sha256"
echo "created ${archive}"
echo "contents from: ${pkg_dir}"
