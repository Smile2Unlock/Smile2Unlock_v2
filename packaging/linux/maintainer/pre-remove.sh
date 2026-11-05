#!/bin/sh
set -eu

# dpkg passes "upgrade"; RPM passes 1 when the package is being replaced.
# Keep PAM integration across upgrades because referenced paths are stable.
case "${1:-}" in
    upgrade|1) exit 0 ;;
esac

helper=/usr/libexec/smile2unlock/su_deploy_helper
if [ -x "$helper" ]; then
    "$helper" --rollback-all
fi
if command -v systemctl >/dev/null 2>&1; then
    systemctl disable --now su-authd.service >/dev/null 2>&1 || true
fi
if [ -d /var/lib/smile2unlock/users ]; then
    find /var/lib/smile2unlock/users -mindepth 2 -maxdepth 2 -type f -name 'authtok.s2u' -delete
fi
/usr/libexec/smile2unlock/manage-selinux-policy remove
