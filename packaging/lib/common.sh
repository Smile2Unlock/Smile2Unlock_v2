#!/usr/bin/env bash

# Shared packaging primitives. Platform scripts source this file after enabling
# `set -euo pipefail`.

packaging_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
project_dir="$(cd "${packaging_dir}/.." && pwd)"
package_name="smile2unlock"
package_version="$(tr -d '[:space:]' < "${project_dir}/version.txt")"

package_die() {
    echo "packaging: $*" >&2
    exit 1
}

package_require_command() {
    command -v "$1" >/dev/null 2>&1 || package_die "required command not found: $1"
}

package_require_file() {
    [[ -f "$1" ]] || package_die "required file not found: $1"
}

package_require_directory() {
    [[ -d "$1" ]] || package_die "required directory not found: $1"
}

package_absolute_path() {
    local value="$1"
    if [[ "$value" == /* ]]; then
        printf '%s\n' "$value"
    else
        printf '%s/%s\n' "$PWD" "$value"
    fi
}

# Only staging directories below build/package-stage may be replaced. This
# prevents a malformed option or empty variable from widening an rm target.
package_reset_stage() {
    local target="$1"
    local stage_root="${project_dir}/build/package-stage"
    [[ "$target" == "${stage_root}/"* && "$target" != "$stage_root" ]] \
        || package_die "refusing to replace unsafe staging directory: $target"
    rm -rf -- "$target"
    mkdir -p -- "$target"
}

package_file_count() {
    find "$1" -type f -print | wc -l
}

package_verify_release_info() {
    local staged_root="$1"
    local release_info="$2"
    package_require_file "$release_info"
    python3 - "$staged_root" "$release_info" "$package_version" <<'PY'
import hashlib, json, os, pathlib, sys

root = pathlib.Path(sys.argv[1]).resolve()
metadata_path = pathlib.Path(sys.argv[2]).resolve()
expected_version = sys.argv[3]
with metadata_path.open(encoding="utf-8") as stream:
    metadata = json.load(stream)
if metadata.get("schema") != 1 or metadata.get("version") != expected_version:
    raise SystemExit("invalid release-info schema or version")
seen = set()
for entry in metadata.get("files", []):
    relative = pathlib.PurePosixPath(entry["path"])
    if relative.is_absolute() or ".." in relative.parts or entry["path"] in seen:
        raise SystemExit(f"invalid release-info path: {entry['path']}")
    seen.add(entry["path"])
    candidate = (root / pathlib.Path(*relative.parts)).resolve()
    if os.path.commonpath((root, candidate)) != str(root) or not candidate.is_file():
        raise SystemExit(f"release-info file missing: {entry['path']}")
    digest = hashlib.sha256()
    with candidate.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != entry.get("sha256"):
        raise SystemExit(f"release-info checksum mismatch: {entry['path']}")
actual = set()
for candidate in root.rglob("*"):
    if (not candidate.is_file() or candidate.resolve() == metadata_path
            or candidate.name == "release-info.p7s"):
        continue
    relative = candidate.relative_to(root).as_posix()
    # Package managers add top-level dot metadata when their archives are
    # extracted. release-info covers the installed filesystem tree.
    if not relative.startswith("."):
        actual.add(relative)
if seen != actual:
    missing = sorted(actual - seen)
    extra = sorted(seen - actual)
    raise SystemExit(
        f"release-info file set mismatch; missing={missing}, extra={extra}")
PY
}
