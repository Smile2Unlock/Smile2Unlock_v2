#!/usr/bin/env bash
# Seed the release MinGW/x86_64 Slint package, including its host compiler.
set -euo pipefail

version=v1.17.0
# Xmake identity for the local recipe's release/static/pic MinGW configuration.
# Regenerate the prebuilt archive and this identity when that configuration changes.
package_hash=6a066fdf9ec8401f9a17206d0f1f9f33
archive_sha256=fcda2e424e539d25dec368a210fc9bc1baf5910f0fe1e9932cc773888f5ebbdc
xmake_home="${XMAKE_GLOBALDIR:-${HOME}/.xmake}"
package_dir="${xmake_home}/packages/s/slint/${version}/${package_hash}"

package_ready() {
    [[ -f "${package_dir}/manifest.txt" &&
       -f "${package_dir}/include/slint/slint.h" &&
       -f "${package_dir}/lib/libslint_cpp.a" &&
       -x "${package_dir}/bin/slint-compiler" ]]
}

archive=""
if (($#)); then
    [[ $# == 2 && "$1" == --archive ]] || {
        echo "usage: $0 [--archive FILE]" >&2; exit 1;
    }
    archive="$2"
fi
if package_ready; then
    echo "Slint already cached: ${package_dir}"
    exit 0
fi

temporary="$(mktemp -d)"
trap 'rm -rf -- "$temporary"' EXIT
if [[ -z "$archive" ]]; then
    archive="${temporary}/slint.tar.gz"
    curl -fsSL --retry 3 -o "$archive" \
        "https://github.com/${GITHUB_REPOSITORY:-Smile2Unlock/Smile2Unlock_v2}/releases/download/slint-prebuilt-${version}/slint-mingw-x86_64-${version}.tar.gz"
fi
actual="$(sha256sum "$archive" | cut -d' ' -f1)"
[[ "$actual" == "$archive_sha256" ]] || {
    echo "Slint prebuilt checksum mismatch" >&2; exit 1;
}
# The archive already contains slint/<version>/<hash>; extract at its parent.
mkdir -p "${xmake_home}/packages/s"
tar -xzf "$archive" -C "${xmake_home}/packages/s"
package_ready || { echo "Slint prebuilt package is incomplete: ${package_dir}" >&2; exit 1; }
echo "Seeded Slint prebuilt: ${package_dir}"
