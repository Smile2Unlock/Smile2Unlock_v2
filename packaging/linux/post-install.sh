#!/bin/sh
set -eu

/usr/libexec/smile2unlock/manage-selinux-policy install

# Image/chroot installation must not operate on the host service manager.
if [ ! -d /run/systemd/system ] || ! command -v systemctl >/dev/null 2>&1; then
    exit 0
fi

systemctl daemon-reload
# A stopped bus reads the new activation files on its next start.
if systemctl --quiet is-active dbus.service; then
    busctl --system call org.freedesktop.DBus /org/freedesktop/DBus \
        org.freedesktop.DBus ReloadConfig >/dev/null
fi

# Replace running processes after all binaries/libraries have been installed.
# Do not start inactive services or change the administrator's enablement.
systemctl try-restart su-authd.service su-deploy-helper.service
