#!/bin/sh
set -eu

helper=/usr/libexec/smile2unlock/su_deploy_helper
if [ -x "$helper" ]; then
    "$helper" --check-package-version @PACKAGE_VERSION@
fi
