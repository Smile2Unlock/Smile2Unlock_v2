#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${BUILD_DIR:-${project_dir}/build/linux/x86_64/release}"
source_user="${SOURCE_USER:-${SUDO_USER:-}}"
acceptance="${build_dir}/su_pam_acceptance"

if [[ "${EUID}" -ne 0 ]]; then
    echo "multi-user-acceptance.sh must run as root" >&2
    exit 1
fi
if [[ -z "${source_user}" || "${source_user}" == "root" ]]; then
    echo "set SOURCE_USER to an existing non-root enrolled user" >&2
    exit 64
fi
if ! getent passwd "${source_user}" >/dev/null; then
    echo "source user does not exist: ${source_user}" >&2
    exit 64
fi
if [[ ! -x "${acceptance}" || ! -f "${build_dir}/pam_smile2unlock.so" ]]; then
    echo "missing acceptance binary or PAM module under ${build_dir}" >&2
    exit 1
fi

source_home="$(getent passwd "${source_user}" | cut -d: -f6)"
source_config="${source_home}/.config/smile2unlock/config.toml"
source_profiles="${source_home}/.local/share/smile2unlock/profiles.json"
[[ -f "${source_config}" ]] || { echo "source config is missing" >&2; exit 1; }
[[ -f "${source_profiles}" ]] || { echo "source profile store is missing" >&2; exit 1; }
source_config_hash="$(sha256sum "${source_config}" | cut -d' ' -f1)"
source_profiles_hash="$(sha256sum "${source_profiles}" | cut -d' ' -f1)"

test_user="smile2unlock-test-${BASHPID}"
test_home="/home/${test_user}"
runtime_dir="/run/${test_user}-runtime"
test_config="${test_home}/.config/smile2unlock/config.toml"
test_profiles="${test_home}/.local/share/smile2unlock/profiles.json"
test_acceptance="${runtime_dir}/su_pam_acceptance"
test_module="${runtime_dir}/pam_smile2unlock.so"
test_uid=""

local_test_user_exists() {
    awk -F: -v username="${test_user}" \
        '$1 == username { found = 1 } END { exit !found }' /etc/passwd
}

cleanup() {
    if [[ -n "${test_uid}" ]]; then
        pkill -KILL -u "${test_uid}" >/dev/null 2>&1 || true
    fi
    for attempt in 1 2 3 4 5; do
        if ! local_test_user_exists; then
            break
        fi
        userdel "${test_user}" >/dev/null 2>&1 || true
        sleep 0.1
    done
    if [[ -d "${test_home}" ]]; then
        find "${test_home}" -mindepth 1 -delete
        rmdir "${test_home}" 2>/dev/null || true
    fi
    if [[ -d "${runtime_dir}" ]]; then
        find "${runtime_dir}" -mindepth 1 -delete
        rmdir "${runtime_dir}" 2>/dev/null || true
    fi
    if local_test_user_exists || [[ -e "${test_home}" ]]; then
        echo "cleanup failed for ${test_user}" >&2
        return 1
    fi
}
trap cleanup EXIT INT TERM

if local_test_user_exists || [[ -e "${test_home}" ]]; then
    echo "refusing to use an existing test account or home: ${test_user}" >&2
    exit 1
fi

useradd --create-home --home-dir "${test_home}" --shell /usr/bin/nologin \
    --user-group "${test_user}"
test_uid="$(id -u "${test_user}")"
install -d -o "${test_user}" -g "${test_user}" -m 0700 "${runtime_dir}"
install -d -o "${test_user}" -g "${test_user}" -m 0700 \
    "${test_home}" \
    "${test_home}/.config" \
    "${test_home}/.config/smile2unlock" \
    "${test_home}/.local" \
    "${test_home}/.local/share" \
    "${test_home}/.local/share/smile2unlock"
install -m 0755 "${acceptance}" "${test_acceptance}"
install -m 0644 "${build_dir}/pam_smile2unlock.so" "${test_module}"

as_test_user() {
    runuser -u "${test_user}" -- env \
        HOME="${test_home}" \
        XDG_RUNTIME_DIR="${runtime_dir}" \
        "$@"
}

expect_status() {
    local expected="$1"
    local label="$2"
    shift 2
    set +e
    "$@"
    local actual=$?
    set -e
    if [[ "${actual}" -ne "${expected}" ]]; then
        echo "${label}: expected exit ${expected}, got ${actual}" >&2
        exit 1
    fi
    echo "ok: ${label} (exit ${actual})"
}

expect_biometric_decision() {
    local label="$1"
    shift
    set +e
    "$@"
    local actual=$?
    set -e
    if [[ "${actual}" -ne 0 && "${actual}" -ne 2 ]]; then
        echo "${label}: expected accepted or biometric rejected, got exit ${actual}" >&2
        exit 1
    fi
    echo "ok: ${label} (exit ${actual})"
}

expect_status 3 "new user without profiles is unavailable" \
    as_test_user "${test_acceptance}" --module "${test_module}" --user "${test_user}"
expect_status 2 "new user cannot request source user" \
    as_test_user "${test_acceptance}" --module "${test_module}" --user "${source_user}"

install -o "${test_user}" -g "${test_user}" -m 0600 "${source_config}" "${test_config}"
install -o "${test_user}" -g "${test_user}" -m 0600 "${source_profiles}" "${test_profiles}"
expect_biometric_decision "new user reaches biometric decision with its copied enrollment" \
    as_test_user "${test_acceptance}" --module "${test_module}" --user "${test_user}"

rm -f "${test_profiles}"
ln -s "${source_profiles}" "${test_profiles}"
expect_status 3 "profile symlink is rejected" \
    as_test_user "${test_acceptance}" --module "${test_module}" --user "${test_user}"

rm -f "${test_profiles}"
install -m 0600 "${source_profiles}" "${test_profiles}"
expect_status 3 "profile owned by the wrong uid is rejected" \
    as_test_user "${test_acceptance}" --module "${test_module}" --user "${test_user}"

chown "${test_user}:${test_user}" "${test_profiles}"
chmod 0666 "${test_profiles}"
expect_status 3 "world-writable profile is rejected" \
    as_test_user "${test_acceptance}" --module "${test_module}" --user "${test_user}"

chmod 0600 "${test_profiles}"
printf '%s\n' '{not-json}' > "${test_profiles}"
expect_status 3 "corrupt profile is unavailable" \
    as_test_user "${test_acceptance}" --module "${test_module}" --user "${test_user}"

install -o "${test_user}" -g "${test_user}" -m 0600 "${source_profiles}" "${test_profiles}"
chmod 0000 "${test_home}"
expect_status 3 "inaccessible home is unavailable" \
    "${acceptance}" --user "${test_user}"
chmod 0700 "${test_home}"

rm -f "${test_profiles}"
expect_status 3 "removed user's profile is unavailable" \
    as_test_user "${test_acceptance}" --module "${test_module}" --user "${test_user}"
[[ "$(sha256sum "${source_config}" | cut -d' ' -f1)" == "${source_config_hash}" ]]
[[ "$(sha256sum "${source_profiles}" | cut -d' ' -f1)" == "${source_profiles_hash}" ]]
echo "ok: source user's config and profiles remain unchanged"

echo "multi-user acceptance passed; ${test_user} will be removed by cleanup"
