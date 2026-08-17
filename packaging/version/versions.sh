#!/usr/bin/env bash
# Shared helpers for the Smile2Unlock version / release management system.
#
# This script is sourced by the packaging scripts (packaging/windows/package.sh
# and packaging/linux/package.sh). It provides:
#
#   version_read()          -> print the current version from version.txt (2.2.0)
#   version_bucket(S)       -> print major.minor bucket ("2.2") for a version
#   ensure_manifest()       -> create the release manifest if missing
#   inject_release_info(ROOT PLATFORM) -> write release-info.json into a staged tree
#   manifest_add(TITLE ARCHIVE)       -> record the release (SHA256) + index
#
# The manifest (packaging/version/releases.json) is the source of truth for
# what has been shipped and is the basis for future incremental updates: each
# entry lists every file shipped and its SHA256, so an updater can diff a
# previous release against the current one and ship only what changed.
#
# Layout:
#   packaging/version/
#     versions.sh                 this file (sourced)
#     releases.json               per-version release records (incl. SHA256)
#     template-release-info.json  schema template

set -euo pipefail

if [[ "${BASH_SOURCE[0]}" == /* ]]; then
    version_dir="$(dirname "${BASH_SOURCE[0]}")"
else
    version_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi
project_dir="$(cd "${version_dir}/../.." && pwd)"

version_file="${project_dir}/version.txt"
manifest_file="${version_dir}/releases.json"
template_file="${version_dir}/template-release-info.json"

version_read() {
    tr -d '[:space:]' < "${version_file}"
}

version_bucket() {
    local version="$1"
    local major minor
    major="${version%%.*}"
    minor="${version#*.}"
    minor="${minor%%.*}"
    printf '%s.%s' "$major" "$minor"
}

ensure_manifest() {
    if [[ ! -f "${manifest_file}" ]]; then
        mkdir -p "$(dirname "${manifest_file}")"
        printf '%s\n' '{"streams":{},"releases":[]}' > "${manifest_file}"
    fi
}

# inject_release_info STAGED_ROOT PLATFORM
# Writes release-info.json (schema/version/stream/date + per-file SHA256) into
# the staged package root. `platform` is one of "windows" or "linux".
inject_release_info() {
    local staged_root="$1"
    local platform="$2"
    ensure_manifest
    mkdir -p "${staged_root}"
    python3 - "$template_file" "$(version_read)" "$(date -u +%Y-%m-%d)" \
        "$platform" "$(uname -m)" "${staged_root}/release-info.json" <<'PY'
import hashlib, json, os, sys
template, version, day, platform, arch, out = sys.argv[1:7]
info = {
    "schema": 1,
    "version": version,
    "stream": ".".join(version.split(".")[:2]),
    "date": day,
    "build": {"platform": platform, "arch": arch},
    "files": [],
}
root = os.path.dirname(out)
for dirpath, _dirnames, filenames in os.walk(root):
    for name in sorted(filenames):
        if name == "release-info.json":
            continue
        full = os.path.join(dirpath, name)
        rel = os.path.relpath(full, root).replace(os.sep, "/")
        with open(full, "rb") as f:
            sha = hashlib.sha256(f.read()).hexdigest()
        info["files"].append({"path": rel, "sha256": sha})
with open(out, "w", encoding="utf-8") as f:
    json.dump(info, f, indent=2, ensure_ascii=False)
    f.write("\n")
PY
    local file_count
    file_count="$(python3 -c "import json;print(len(json.load(open('${staged_root}/release-info.json'))['files']))")"
    echo "release-info: ${platform} $(version_read) -> ${file_count} files"
}

# manifest_add TITLE ARCHIVE
# Computes the SHA256 of the shipped archive, appends a release record to
# releases.json and refreshes the per-stream index.
manifest_add() {
    local title="$1"
    local archive="$2"
    ensure_manifest
    local version
    version="$(version_read)"
    local bucket
    bucket="$(version_bucket "${version}")"
    local sha=""
    [[ -f "${archive}" ]] && sha="$(sha256sum "${archive}" | awk '{print $1}')"
    local size_kb
    size_kb="$(du -sk "${archive}" 2>/dev/null | awk '{print $1}')"
    local day
    day="$(date -u +%Y-%m-%d)"
    local tmp
    tmp="$(mktemp)"
    python3 - "$manifest_file" "$version" "$bucket" "$day" "$title" \
        "$(basename "${archive}")" "$sha" "${size_kb:-0}" > "$tmp" <<'PY'
import json, sys
path, version, bucket, day, title, archive, sha, size_kb = sys.argv[1:9]
with open(path, encoding="utf-8") as f:
    doc = json.load(f)
entry = {
    "version": version, "stream": bucket, "date": day, "title": title,
    "archive": archive, "sha256": sha, "size_kb": int(size_kb),
    "incremental_from": None,
}
releases = [r for r in doc.get("releases", [])
            if not (r.get("stream") == bucket and r.get("version") == version
                    and r.get("archive") == archive)]
releases.append(entry)
releases.sort(key=lambda r: (r.get("stream", ""), r.get("version", "")))
doc["releases"] = releases
index = {}
for r in releases:
    index[r["stream"]] = r["version"]
doc.setdefault("streams", {}).update(index)
with open(path, "w", encoding="utf-8") as f:
    json.dump(doc, f, indent=2, ensure_ascii=False)
    f.write("\n")
PY
    rm -f "$tmp"
    echo "manifest: recorded ${bucket}:${version} (${sha:-no-sha})"
}
