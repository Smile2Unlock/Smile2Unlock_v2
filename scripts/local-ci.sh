#!/usr/bin/env bash
# 用 act + Docker 在本地复现 .github/workflows/build-release-readiness.yml。
#
# 用法：
#   scripts/local-ci.sh                 # 依次运行全部三个 job
#   scripts/local-ci.sh linux           # 只运行 linux job
#   scripts/local-ci.sh windows-cross windows-package
#
# 说明：
# - 工作副本会被 rsync 到临时目录（排除 build/、Rust target/ 等本地产物），
#   等效于 GitHub 上的干净 checkout，避免复用旧构建产物。
# - 需要本机 Docker；act 与 catthehacker/ubuntu:act-24.04、archlinux:base-devel
#   镜像会在缺失时自动准备。
# - 与 GitHub 托管 runner 的差异：checkout 步骤是 docker cp 本地副本而不是
#   重新克隆；actions/cache 不生效；除此之外步骤与 workflow 一致。

set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
workflow="${repo_dir}/.github/workflows/build-release-readiness.yml"
platform="ubuntu-24.04=catthehacker/ubuntu:act-24.04"
arch_image="archlinux:base-devel"
act_image="catthehacker/ubuntu:act-24.04"
copy_dir="${LOCAL_CI_COPY_DIR:-/tmp/s2u-ci-src}"
log_dir="${LOCAL_CI_LOG_DIR:-/tmp/s2u-local-ci}"
# Hosted runners cache ~/.xmake/packages via actions/cache, which act does not
# honor; without a registry cache every local run re-downloads the whole Slint
# dependency graph, which is slow and exposes the run to transient crates.io
# failures. Persist the Cargo registry and the pacman package cache in named
# volumes instead, so a retry after a flaky mirror resumes instead of
# re-downloading everything.
cargo_volume="${LOCAL_CI_CARGO_VOLUME:-s2u-cargo-registry}"
pacman_volume="${LOCAL_CI_PACMAN_VOLUME:-s2u-pacman-cache}"

all_jobs=(linux windows-cross windows-package)
jobs=("$@")
((${#jobs[@]} > 0)) || jobs=("${all_jobs[@]}")
for job in "${jobs[@]}"; do
    case "$job" in linux|windows-cross|windows-package) ;; *)
        echo "unknown job: $job (expected: ${all_jobs[*]})" >&2; exit 2 ;;
    esac
done

for command in act docker rsync; do
    command -v "$command" >/dev/null || { echo "missing dependency: $command" >&2; exit 2; }
done

docker image inspect "$arch_image" >/dev/null 2>&1 || docker pull "$arch_image"
docker image inspect "$act_image" >/dev/null 2>&1 || docker pull "$act_image"
docker volume inspect "$cargo_volume" >/dev/null 2>&1 || docker volume create "$cargo_volume" >/dev/null
docker volume inspect "$pacman_volume" >/dev/null 2>&1 || docker volume create "$pacman_volume" >/dev/null

# act shares the `act-toolcache` docker volume across runs. xmake built inside
# the archlinux container (glibc 2.43) cannot run in the ubuntu 24.04 container
# (glibc 2.39), so drop cached xmake binaries before every job. Do not run two
# act jobs in parallel: one job's cached toolchain leaks into the other.
wipe_toolcache_xmake() {
    docker volume inspect act-toolcache >/dev/null 2>&1 \
        && docker run --rm -v act-toolcache:/toolcache "$act_image" \
            bash -c 'rm -rf /toolcache/xmake' || true
}

mkdir -p "$log_dir"
rsync -a --delete \
    --exclude='.git' --exclude='build' --exclude='cmake-build-debug' \
    --exclude='compile_commands.json' --exclude='.xmake' --exclude='target' \
    --exclude='.cache' --exclude='.claude' --exclude='.codewhale' \
    --exclude='.codex' --exclude='.deepseek' --exclude='.idea' --exclude='.zcode' \
    "$repo_dir/" "$copy_dir/"

status=0
for job in "${jobs[@]}"; do
    log="${log_dir}/${job}.log"
    wipe_toolcache_xmake
    echo "==> running job '${job}' (log: ${log})"
    if (cd "$copy_dir" && act push --pull=false -W "$workflow" -j "$job" \
            -P "$platform" \
            --container-options "-v ${cargo_volume}:/root/.cargo/registry -v ${pacman_volume}:/var/cache/pacman/pkg" \
            >| "$log" 2>&1); then
        echo "==> job '${job}' PASSED"
    else
        echo "==> job '${job}' FAILED (see ${log})"
        status=1
    fi
done

exit "$status"
