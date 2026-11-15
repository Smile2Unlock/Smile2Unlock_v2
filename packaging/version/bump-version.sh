#!/usr/bin/env bash
# Set the local development version fallback.
#
#   packaging/version/bump-version.sh 2.4.2
#
# Official release versions come from pushed vMAJOR.MINOR.PATCH tags. This
# helper only updates version.txt for local builds without SU_VERSION.
#
# Xmake generates target versions, C++ defines and Windows resources from
# version.txt only when SU_VERSION is not provided.

set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version_file="${project_dir}/version.txt"

usage() {
    echo "Usage: packaging/version/bump-version.sh <version> [--no-tag]"
    echo "Sets the local version.txt fallback; releases are created by pushing vX.Y.Z tags."
    exit 1
}

[[ $# -ge 1 ]] || usage
new_version="$1"
shift
[[ "$new_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
    echo "invalid version (want MAJOR.MINOR.PATCH): $new_version" >&2
    exit 1
}
[[ -f "$version_file" ]] && old_version="$(tr -d '[:space:]' < "$version_file")" || old_version="(none)"

while (($# > 0)); do
    case "$1" in
        --no-tag) shift ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done

printf '%s\n' "${new_version}" > "${version_file}"

echo "development version: ${old_version:-<empty>} -> ${new_version}"
echo "release version is set by pushing v${new_version} after the release commit is on main"
