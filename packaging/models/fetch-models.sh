#!/usr/bin/env bash
# Download and verify the pinned LightFace ONNX models from the
# LightFacePipeline GitHub Release so CI and source builds without Git LFS
# still obtain verified model weights.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "${script_dir}/../.." && pwd)"
manifest="${script_dir}/lightface-v1.json"
sums_file="${project_dir}/assets/models/lightface/SHA256SUMS"

cache_dir="${XDG_CACHE_HOME:-${HOME}/.cache}/smile2unlock/models"
target_dir="${project_dir}/assets/models/lightface"

usage() {
    cat <<EOF
Usage: $0 [--cache-dir DIR] [--target-dir DIR]
EOF
}

while (($#)); do
    case "$1" in
        --cache-dir)
            [[ $# -ge 2 ]] || { usage >&2; exit 1; }
            cache_dir="$2"
            shift 2
            ;;
        --target-dir)
            [[ $# -ge 2 ]] || { usage >&2; exit 1; }
            target_dir="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 1
            ;;
    esac
done

[[ -f "$manifest" ]] || { echo "models: missing manifest ${manifest}" >&2; exit 1; }
[[ -f "$sums_file" ]] || { echo "models: missing checksums ${sums_file}" >&2; exit 1; }

models_verified() {
    [[ -d "$target_dir" ]] && (
        cd "$target_dir" && sha256sum -c "$sums_file" >/dev/null 2>&1
    )
}

if models_verified; then
    echo "LightFace models already verified: ${target_dir}"
    exit 0
fi

mapfile -t model_entries < <(
    python3 - "$manifest" <<'PY'
import json, sys
info = json.load(open(sys.argv[1]))
repo = info['repository']
tag = info['tag']
for name, digest in info['files'].items():
    print(f"{repo}\t{tag}\t{name}\t{digest}")
PY
)
[[ ${#model_entries[@]} -gt 0 ]] || {
    echo "models: no files listed in ${manifest}" >&2
    exit 1
}

temporary="$(mktemp -d)"
trap 'rm -rf -- "$temporary"' EXIT

verify_file_sha256() {
    local file="$1"
    local expected="$2"
    local actual
    actual="$(sha256sum "$file" | cut -d' ' -f1)"
    [[ "$actual" == "$expected" ]]
}

mkdir -p -- "$target_dir"

for entry in "${model_entries[@]}"; do
    IFS=$'\t' read -r repository tag name expected_sha <<< "$entry"
    model_cache="${cache_dir}/${tag}"
    mkdir -p -- "$model_cache"
    cached_file="${model_cache}/${name}"
    if [[ ! -f "$cached_file" ]] || ! verify_file_sha256 "$cached_file" "$expected_sha"; then
        url="https://github.com/${repository}/releases/download/${tag}/${name}"
        downloaded="${temporary}/${name}.part"
        echo "Fetching ${name} from ${url}"
        curl -fsSL --retry 3 -o "$downloaded" "$url"
        verify_file_sha256 "$downloaded" "$expected_sha" || {
            echo "models: checksum mismatch for ${url}" >&2
            exit 1
        }
        mv -f -- "$downloaded" "$cached_file"
    else
        echo "Using cached ${name}: ${cached_file}"
    fi
    install -m 0644 "$cached_file" "${target_dir}/${name}"
done

(cd "$target_dir" && sha256sum -c "$sums_file" >/dev/null) || {
    echo "models: per-file SHA-256 verification failed in ${target_dir}" >&2
    exit 1
}

echo "Fetched and verified LightFace models into ${target_dir}"
