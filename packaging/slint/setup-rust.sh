#!/usr/bin/env bash
# Both Rust staticlibs in the GUI must use the prebuilt Slint compiler version.
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
toolchain="$(python3 - "${script_dir}/prebuilt-mingw.json" <<'PY'
import json, sys
print(json.load(open(sys.argv[1]))['rust_toolchain'])
PY
)"
if ! rustup toolchain install "$toolchain" --profile minimal "$@"; then
    # Some mirrors omit pinned releases; use the official archive as fallback.
    RUSTUP_DIST_SERVER=https://static.rust-lang.org \
        rustup toolchain install "$toolchain" --profile minimal "$@"
fi
export RUSTUP_TOOLCHAIN="$toolchain"
if [[ -n "${GITHUB_ENV:-}" ]]; then
    printf 'RUSTUP_TOOLCHAIN=%s\n' "$toolchain" >> "$GITHUB_ENV"
fi
echo "Prebuilt Slint Rust toolchain: ${toolchain}"
