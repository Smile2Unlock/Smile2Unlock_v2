#!/usr/bin/env bash
# Seed the release MinGW/x86_64 Slint package, including its host compiler.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Archive identity and Rust version must be regenerated together.
read -r version package_hash archive_sha256 rust_commit archive_name repository < <(
    python3 - "${script_dir}/prebuilt-mingw.json" <<'PY'
import json, sys
info = json.load(open(sys.argv[1]))
print(info['version'], info['package_hash'], info['archive_sha256'], info['rust_commit'], info['archive_name'], info['repository'])
PY
)
actual_rust_commit="$(rustc -Vv | sed -n 's/^commit-hash: //p')"
[[ "$actual_rust_commit" == "$rust_commit" ]] || {
    echo "Slint prebuilt requires Rust ${rust_commit}; select the toolchain in prebuilt-mingw.json" >&2
    exit 1
}
# Xmake appends .xmake to XMAKE_GLOBALDIR (or HOME).
xmake_home="${XMAKE_GLOBALDIR:-${HOME}}/.xmake"
package_dir="${xmake_home}/packages/s/slint/${version}/${package_hash}"

package_ready() {
    [[ -f "${package_dir}/manifest.txt" &&
       -f "${package_dir}/include/slint/slint.h" &&
       -f "${package_dir}/lib/libslint_cpp.a" &&
       -x "${package_dir}/bin/slint-compiler" ]]
}

cache_ready() {
    package_ready && [[ -f "${package_dir}/.su-prebuilt-sha256" ]] &&
        [[ "$(cat "${package_dir}/.su-prebuilt-sha256")" == "$archive_sha256" ]]
}

archive=""
if (($#)); then
    [[ $# == 2 && "$1" == --archive ]] || {
        echo "usage: $0 [--archive FILE]" >&2; exit 1;
    }
    archive="$2"
fi
if cache_ready; then
    echo "Slint already cached: ${package_dir}"
    exit 0
fi

temporary="$(mktemp -d)"
trap 'rm -rf -- "$temporary"' EXIT
if [[ -z "$archive" ]]; then
    archive="${temporary}/slint.tar.gz"
    curl -fsSL --retry 3 -o "$archive" \
        "https://github.com/${repository}/releases/download/slint-prebuilt-${version}/${archive_name}"
fi
actual="$(sha256sum "$archive" | cut -d' ' -f1)"
[[ "$actual" == "$archive_sha256" ]] || {
    echo "Slint prebuilt checksum mismatch" >&2; exit 1;
}
# The archive already contains slint/<version>/<hash>; extract at its parent.
mkdir -p "${xmake_home}/packages/s"
tar -xzf "$archive" -C "${xmake_home}/packages/s"
package_ready || { echo "Slint prebuilt package is incomplete: ${package_dir}" >&2; exit 1; }
printf '%s\n' "$archive_sha256" > "${package_dir}/.su-prebuilt-sha256"
echo "Seeded Slint prebuilt: ${package_dir}"
