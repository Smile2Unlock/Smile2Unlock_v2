#!/usr/bin/env bash
set -euo pipefail

credential_dir="${CREDENTIAL_DIR:-/etc/credstore.encrypted}"
credential_name="smile2unlock-master.key"
credential_path="${credential_dir}/${credential_name}"

if [[ "${EUID}" -ne 0 ]]; then
    echo "setup-storage-key must run as root" >&2
    exit 1
fi

if [[ -e "${credential_path}" ]]; then
    [[ -f "${credential_path}" && ! -L "${credential_path}" ]] || {
        echo "unsafe existing credential path: ${credential_path}" >&2
        exit 1
    }
    echo "Smile2Unlock storage credential already exists; keeping the current key."
    exit 0
fi

command -v systemd-creds >/dev/null || {
    echo "systemd-creds is required for encrypted profile storage" >&2
    exit 1
}
command -v systemd-analyze >/dev/null || {
    echo "systemd-analyze is required for TPM2 detection" >&2
    exit 1
}

systemd-creds setup
tpm_report="$(systemd-analyze has-tpm2 2>/dev/null || true)"
if [[ "${tpm_report%%$'\n'*}" == "yes" ]]; then
    protection_id=1
    key_mode="host+tpm2"
    protection_name="TPM2-bound"
else
    protection_id=2
    key_mode="host"
    protection_name="host-key"
fi

install -d -m 0700 "${credential_dir}"
temporary="$(mktemp "${credential_dir}/.${credential_name}.XXXXXX")"
cleanup() {
    rm -f "${temporary}"
}
trap cleanup EXIT

{
    printf 'S2UK\x00\x01'
    printf "\\x$(printf '%02x' "${protection_id}")"
    printf '\x00\x00\x00\x00\x01'
    head -c 32 /dev/urandom
} | systemd-creds --name="${credential_name}" --newline=no \
    --with-key="${key_mode}" encrypt - "${temporary}"

chmod 0600 "${temporary}"
mv -T "${temporary}" "${credential_path}"
trap - EXIT
echo "Created ${protection_name} Smile2Unlock storage credential."
