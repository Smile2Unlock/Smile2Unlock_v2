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
# Output: build/packages/<archive_name from prebuilt-mingw.json>
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
read -r version package_hash rust_commit archive_name < <(
    python3 - "${project_dir}/packaging/slint/prebuilt-mingw.json" <<'PY'
import json, sys
info = json.load(open(sys.argv[1]))
print(info['version'], info['package_hash'], info['rust_commit'], info['archive_name'])
PY
)
pkg_dir="${XMAKE_GLOBALDIR:-${HOME}}/.xmake/packages/s/slint/${version}/${package_hash}"
[[ -f "${pkg_dir}/lib/libslint_cpp.a" && -x "${pkg_dir}/bin/slint-compiler" ]] || {
    echo "slint: installed mingw package not found under ${pkg_dir};" >&2
    echo "build it first: xmake f -p mingw ... && xmake build su_app" >&2
    exit 1
}
# Check the compiler embedded in the actual library, not the current shell.
strings "${pkg_dir}/lib/libslint_cpp.a" | grep -F "/rustc/${rust_commit}/" >/dev/null || {
    echo "slint: rebuild with the Rust toolchain in prebuilt-mingw.json before packaging" >&2
    exit 1
}

out="${project_dir}/build/packages"
mkdir -p -- "${out}"
archive="${out}/${archive_name}"
tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp}"' EXIT
# Preserve the source directory's hash name: xmake resolves the package to
# the directory whose hash matches recipe+config, and our mingw config is
# what CI reproduces, so the same name makes the seed resolve cleanly.
hash_name="$(basename "${pkg_dir}")"
mkdir -p "${tmp}/slint/${version}/${hash_name}"
cp -r "${pkg_dir}/." "${tmp}/slint/${version}/${hash_name}/"
rm -f "${tmp}/slint/${version}/${hash_name}/.su-prebuilt-sha256"
( cd "${tmp}" && tar -czf "${archive}" slint )
( cd "${out}" && sha256sum "${archive_name}" > "${archive}.sha256" )
echo "created ${archive}"
echo "contents from: ${pkg_dir}"
