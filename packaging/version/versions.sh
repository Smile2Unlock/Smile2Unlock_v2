#!/usr/bin/env bash
# Shared helpers for Smile2Unlock version and package metadata.
#
# This script is sourced by the packaging scripts (packaging/windows/package.sh
# and packaging/linux/package.sh). It provides:
#
#   version_read()          -> print the current version from version.txt (2.2.0)
#   inject_release_info(ROOT PLATFORM) -> write release-info.json into a staged tree
#
# Layout:
#   packaging/version/
#     versions.sh                 this file (sourced)
#     template-release-info.json  schema template

set -euo pipefail

if [[ "${BASH_SOURCE[0]}" == /* ]]; then
    version_dir="$(dirname "${BASH_SOURCE[0]}")"
else
    version_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi
project_dir="$(cd "${version_dir}/../.." && pwd)"

version_file="${project_dir}/version.txt"
template_file="${version_dir}/template-release-info.json"

version_read() {
    tr -d '[:space:]' < "${version_file}"
}

# inject_release_info STAGED_ROOT PLATFORM [OUTPUT]
# Writes release-info.json (schema/version/stream/date + per-file SHA256).
# OUTPUT may be inside STAGED_ROOT, allowing Linux to keep metadata under
# /usr/share while still covering the complete package tree.
inject_release_info() {
    local staged_root="$1"
    local platform="$2"
    local output="${3:-${staged_root}/release-info.json}"
    local architecture="${PACKAGE_ARCH:-$(uname -m)}"
    mkdir -p "${staged_root}" "$(dirname "${output}")"
    python3 - "$template_file" "$(version_read)" "$(date -u +%Y-%m-%d)" \
        "$platform" "$architecture" "${staged_root}" "$output" <<'PY'
import hashlib, json, os, sys
template, version, day, platform, arch, root, out = sys.argv[1:8]
info = {
    "schema": 1,
    "version": version,
    "stream": ".".join(version.split(".")[:2]),
    "date": day,
    "build": {"platform": platform, "arch": arch},
    "files": [],
}
out_abs = os.path.abspath(out)
for dirpath, dirnames, filenames in os.walk(root):
    dirnames.sort()
    for name in sorted(filenames):
        full = os.path.abspath(os.path.join(dirpath, name))
        if full == out_abs:
            continue
        rel = os.path.relpath(full, root).replace(os.sep, "/")
        with open(full, "rb") as f:
            digest = hashlib.sha256()
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                digest.update(chunk)
            sha = digest.hexdigest()
        info["files"].append({"path": rel, "sha256": sha})
with open(out, "w", encoding="utf-8") as f:
    json.dump(info, f, indent=2, ensure_ascii=False)
    f.write("\n")
PY
    local file_count
    file_count="$(python3 -c "import json;print(len(json.load(open('${output}'))['files']))")"
    echo "release-info: ${platform} $(version_read) -> ${file_count} files"
}
