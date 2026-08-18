#!/usr/bin/env bash
# Bump the Smile2Unlock version and create a git tag.
#
#   packaging/version/bump-version.sh 2.3.0 [--no-tag]
#
# - Writes version.txt and synchronizes xmake.lua's standalone fallback
# - Runs `git tag v<version>` (unless --no-tag)
# - Prints the next packaging command
#
# version.txt remains the release source of truth; this script synchronizes
# xmake's top-level fallback and packaging reads the file for artifact naming.

set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version_file="${project_dir}/version.txt"
xmake_file="${project_dir}/xmake.lua"

usage() {
    echo "Usage: packaging/version/bump-version.sh <version> [--no-tag]"
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

tag=true
while (($# > 0)); do
    case "$1" in
        --no-tag) tag=false; shift ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done

printf '%s\n' "${new_version}" > "${version_file}"
# Keep the standalone xmake fallback synchronized for callers that do not
# export SU_VERSION.
sed -i -E 's/_su_version = "[0-9]+\.[0-9]+\.[0-9]+"/_su_version = "'"${new_version}"'"/' "${xmake_file}"
echo "version: ${old_version:-<empty>} -> ${new_version}"

if [[ "$tag" == true ]]; then
    # Reuse an existing tag body if present, else a generic one.
    if git -C "$project_dir" rev-parse "v${new_version}" >/dev/null 2>&1; then
        echo "note: tag v${new_version} already exists (skipped)"
    else
        git -C "$project_dir" add version.txt xmake.lua
        git -C "$project_dir" commit -m "chore: bump version to ${new_version}" >/dev/null
        git -C "$project_dir" tag "v${new_version}"
        echo "tagged: v${new_version}"
    fi
fi

echo "next: packaging/windows/package.sh && packaging/linux/package.sh --format all"
