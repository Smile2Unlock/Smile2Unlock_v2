#!/bin/sh
set -eu

case "${1:-}" in
    install|remove) operation=$1 ;;
    *) echo 'usage: manage-selinux-policy install|remove' >&2; exit 2 ;;
esac

# Non-SELinux installations do not need policy tools or a loaded module.
if ! command -v selinuxenabled >/dev/null 2>&1 || ! selinuxenabled; then
    exit 0
fi
for tool in semodule restorecon; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Smile2Unlock SELinux policy requires $tool" >&2
        exit 1
    }
done

# Priority 200 is reserved here for the packaged policy. Administrator modules
# at priority 400 are preserved on both upgrades and removal.
case "$operation" in
    install)
        semodule -X 200 -i /usr/share/smile2unlock/selinux/smile2unlock.cil
        ;;
    remove)
        modules=$(semodule -lfull)
        if printf '%s\n' "$modules" | awk '$1 == 200 && $2 == "smile2unlock" { found=1 } END { exit !found }'; then
            semodule -X 200 -r smile2unlock
        fi
        ;;
esac
if [ -d /run/smile2unlock ]; then
    restorecon -R /run/smile2unlock
fi
