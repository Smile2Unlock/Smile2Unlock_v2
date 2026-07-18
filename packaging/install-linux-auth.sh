#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${project_dir}/build/linux/x86_64/release}"
pam_module_dir="${PAM_MODULE_DIR:-/usr/lib/security}"

if [[ "${EUID}" -ne 0 ]]; then
    echo "install-linux-auth.sh must run as root" >&2
    exit 1
fi

for file in "${build_dir}/su_authd" "${build_dir}/pam_smile2unlock.so"; do
    if [[ ! -f "${file}" ]]; then
        echo "missing build artifact: ${file}" >&2
        exit 1
    fi
done

install -d -m 0755 /usr/libexec/smile2unlock
install -m 0755 "${build_dir}/su_authd" /usr/libexec/smile2unlock/su_authd

install -d -m 0755 "${pam_module_dir}"
install -m 0755 "${build_dir}/pam_smile2unlock.so" \
    "${pam_module_dir}/pam_smile2unlock.so"

install -d -m 0755 /usr/share/smile2unlock/models
install -m 0644 "${build_dir}/assets/models/seeta/"*.csta \
    /usr/share/smile2unlock/models/

seeta_library="$(ldd "${build_dir}/su_authd" \
    | awk '/seetaface6open/ { print $3; exit }')"
if [[ -z "${seeta_library}" || ! -f "${seeta_library}" ]]; then
    echo "failed to locate SeetaFace runtime libraries" >&2
    exit 1
fi
install -d -m 0755 /usr/lib/smile2unlock
install -m 0755 "$(dirname "${seeta_library}")/"*.so* /usr/lib/smile2unlock/

install -m 0644 "${project_dir}/packaging/systemd/su-authd.service" \
    /usr/lib/systemd/system/su-authd.service

systemctl daemon-reload
systemctl enable --now su-authd.service

echo "su-authd installed and started. Add pam_smile2unlock.so to the desired PAM stack."
