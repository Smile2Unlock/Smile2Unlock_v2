#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${project_dir}/build/linux/x86_64/release}"
pam_module_dir="${PAM_MODULE_DIR:-/usr/lib/security}"
systemd_unit_dir="${SYSTEMD_UNIT_DIR:-/usr/lib/systemd/system}"
destination_root="${DESTDIR:-}"

if [[ -z "${destination_root}" && "${EUID}" -ne 0 ]]; then
    echo "install-linux-auth.sh must run as root" >&2
    exit 1
fi

destination_root="${destination_root%/}"
destination() {
    printf '%s%s' "${destination_root}" "$1"
}

for file in "${build_dir}/su_authd" "${build_dir}/pam_smile2unlock.so"; do
    if [[ ! -f "${file}" ]]; then
        echo "missing build artifact: ${file}" >&2
        exit 1
    fi
done

install -d -m 0755 "$(destination /usr/libexec/smile2unlock)"
install -m 0755 "${build_dir}/su_authd" \
    "$(destination /usr/libexec/smile2unlock/su_authd)"
install -m 0755 "${project_dir}/packaging/install-dms-lock.sh" \
    "$(destination /usr/libexec/smile2unlock/install-dms-lock)"

install -d -m 0755 "$(destination "${pam_module_dir}")"
install -m 0755 "${build_dir}/pam_smile2unlock.so" \
    "$(destination "${pam_module_dir}/pam_smile2unlock.so")"

install -d -m 0755 "$(destination /usr/share/smile2unlock/models)"
install -m 0644 "${build_dir}/assets/models/seeta/"*.csta \
    "$(destination /usr/share/smile2unlock/models)/"
install -d -m 0755 "$(destination /usr/share/smile2unlock/pam)"
install -m 0644 "${project_dir}/packaging/linux/pam/dankshell-smile2unlock" \
    "$(destination /usr/share/smile2unlock/pam/dankshell-smile2unlock)"

rpath="$(readelf -d "${build_dir}/su_authd" \
    | sed -n 's/.*Library rpath: \[\(.*\)\]/\1/p')"
IFS=: read -r -a rpath_directories <<< "${rpath}"
seeta_library_dir=""
for directory in "${rpath_directories[@]}"; do
    if [[ "${directory}" == *seetaface6open* \
        && -f "${directory}/libSeetaFaceDetector600.so" ]]; then
        seeta_library_dir="${directory}"
        break
    fi
done
if [[ -z "${seeta_library_dir}" ]]; then
    echo "failed to locate SeetaFace runtime libraries" >&2
    exit 1
fi
shopt -s nullglob
seeta_libraries=(
    "${seeta_library_dir}/"libSeetaAuthorize.so*
    "${seeta_library_dir}/"libSeetaFaceAntiSpoofingX600.so*
    "${seeta_library_dir}/"libSeetaFaceDetector600.so*
    "${seeta_library_dir}/"libSeetaFaceLandmarker600.so*
    "${seeta_library_dir}/"libSeetaFaceRecognizer610.so*
    "${seeta_library_dir}/"libtennis.so*
    "${seeta_library_dir}/"libtennis_haswell*.so*
    "${seeta_library_dir}/"libtennis_pentium*.so*
    "${seeta_library_dir}/"libtennis_sandy_bridge*.so*
)
shopt -u nullglob
if [[ "${#seeta_libraries[@]}" -lt 9 ]]; then
    echo "incomplete SeetaFace runtime library set" >&2
    exit 1
fi
install -d -m 0755 "$(destination /usr/lib/smile2unlock)"
install -m 0755 "${seeta_libraries[@]}" \
    "$(destination /usr/lib/smile2unlock)/"

install -d -m 0755 "$(destination "${systemd_unit_dir}")"
install -m 0644 "${project_dir}/packaging/systemd/su-authd.service" \
    "$(destination "${systemd_unit_dir}/su-authd.service")"

if [[ -z "${destination_root}" ]]; then
    systemctl daemon-reload
    systemctl enable --now su-authd.service
    echo "su-authd installed and started. Add pam_smile2unlock.so to the desired PAM stack."
else
    echo "su-authd staged under ${destination_root}."
fi
