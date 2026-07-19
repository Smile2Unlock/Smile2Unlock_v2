#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_template="${script_dir}/linux/pam/dankshell-smile2unlock"
installed_template="/usr/share/smile2unlock/pam/dankshell-smile2unlock"
template="${DMS_PAM_TEMPLATE:-${project_template}}"
destination_root="${DESTDIR:-}"
operation="install"
force=false

usage() {
    cat <<'USAGE'
Usage: install-dms-lock.sh [--install | --remove] [--force]

Installs or removes /etc/pam.d/dankshell-smile2unlock. This script does not
change the per-user DMS lockPamPath setting.

Environment:
  DESTDIR           Stage files below this root without requiring root
  DMS_PAM_TEMPLATE  Override the PAM template path
USAGE
}

while (($# > 0)); do
    case "$1" in
        --install)
            operation="install"
            ;;
        --remove)
            operation="remove"
            ;;
        --force)
            force=true
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage >&2
            exit 64
            ;;
    esac
    shift
done

if [[ ! -f "${template}" && -f "${installed_template}" ]]; then
    template="${installed_template}"
fi
if [[ "${operation}" == "install" && ! -f "${template}" ]]; then
    echo "missing DMS PAM template: ${template}" >&2
    exit 1
fi
if [[ -z "${destination_root}" && "${EUID}" -ne 0 ]]; then
    echo "install-dms-lock.sh must run as root" >&2
    exit 1
fi

destination_root="${destination_root%/}"
destination="${destination_root}/etc/pam.d/dankshell-smile2unlock"

if [[ "${operation}" == "remove" ]]; then
    if [[ ! -e "${destination}" ]]; then
        echo "DMS Smile2Unlock PAM service is already absent."
        exit 0
    fi
    if [[ "${force}" != true && -f "${template}" ]] \
        && ! cmp -s "${template}" "${destination}"; then
        echo "refusing to remove a modified PAM service; pass --force to override" >&2
        exit 1
    fi
    rm -f "${destination}"
    echo "removed ${destination}"
    exit 0
fi

install -d -m 0755 "${destination_root}/etc/pam.d"
if [[ -e "${destination}" ]] && ! cmp -s "${template}" "${destination}"; then
    if [[ "${force}" != true ]]; then
        echo "refusing to overwrite modified PAM service; pass --force to override" >&2
        exit 1
    fi
    backup_dir="${destination_root}/var/lib/smile2unlock/pam-backups"
    install -d -m 0700 "${backup_dir}"
    backup="${backup_dir}/dankshell-smile2unlock.$(date -u +%Y%m%dT%H%M%SZ)"
    cp -a "${destination}" "${backup}"
    echo "backed up existing PAM service to ${backup}"
fi

install -m 0644 "${template}" "${destination}"
echo "installed ${destination}"
if [[ -z "${destination_root}" ]]; then
    echo "Next, run as the desktop user:"
    echo "  dms auth validate --path /etc/pam.d/dankshell-smile2unlock --json"
    echo "  dms ipc call settings set lockPamPath /etc/pam.d/dankshell-smile2unlock"
fi
