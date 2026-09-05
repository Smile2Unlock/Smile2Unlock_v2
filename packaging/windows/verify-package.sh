#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/common.sh
source "${script_dir}/../lib/common.sh"
# shellcheck source=system-dlls.sh
source "${script_dir}/system-dlls.sh"

[[ $# -eq 1 ]] || package_die "usage: packaging/windows/verify-package.sh PACKAGE.zip"
archive="$(package_absolute_path "$1")"
package_require_file "$archive"
package_require_command unzip
package_require_command x86_64-w64-mingw32-objdump

python3 - "$archive" <<'PY'
import pathlib, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    for name in archive.namelist():
        path = pathlib.PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts or not path.parts or path.parts[0] != "Smile2Unlock":
            raise SystemExit(f"unsafe or unexpected zip path: {name}")
PY

verify_root="${project_dir}/build/package-verify/windows"
package_reset_stage "${project_dir}/build/package-stage/verify-windows"
verify_root="${project_dir}/build/package-stage/verify-windows"
unzip -q "$archive" -d "$verify_root"
root="${verify_root}/Smile2Unlock"

required=(
    bin/su_app.exe
    bin/su_deploy_helper.exe
    bin/su_password_tool.exe
    bin/su_recognition_agent.exe
    bin/su_credential_provider.dll
    bin/Smile2UnlockAuthService.exe
    bin/Smile2Unlock.ico
    assets/i18n/en.json
    assets/i18n/zh-CN.json
    assets/models/seeta/face_detector.csta
    assets/models/seeta/face_landmarker_pts5.csta
    assets/models/seeta/face_recognizer.csta
    assets/models/seeta/fas_first.csta
    assets/models/seeta/fas_second.csta
    release-info.json
    release-info.p7s
)
for path in "${required[@]}"; do
    package_require_file "${root}/${path}"
done

if find "${root}/bin" -maxdepth 1 -type f -name 'su_credential_provider-*.dll' -print -quit \
    | grep -q .; then
    package_die "credential provider must not use a suffixed filename"
fi

for binary in "${root}/bin/"*.exe "${root}/bin/su_credential_provider.dll"; do
    x86_64-w64-mingw32-objdump -f "$binary" | grep -q 'pei-x86-64' \
        || package_die "not an x86_64 Windows binary: $binary"
    while IFS= read -r dependency; do
        windows_is_system_dll "$dependency" && continue
        find "${root}/bin" -maxdepth 1 -type f -iname "$dependency" -print -quit | grep -q . \
            || package_die "missing packaged dependency for $(basename "$binary"): $dependency"
    done < <(x86_64-w64-mingw32-objdump -p "$binary" 2>/dev/null \
        | sed -n 's/^[[:space:]]*DLL Name: \(.*\)/\1/p')
done

package_require_command openssl
openssl cms -verify -binary -inform DER -in "${root}/release-info.p7s" \
    -content "${root}/release-info.json" -noverify -out /dev/null 2>/dev/null \
    || package_die "release-info detached signature is invalid"
if command -v osslsigncode >/dev/null 2>&1; then
    authenticode_verify_args=()
    if [[ -n "${WINDOWS_VERIFY_CA_FILE:-}" ]]; then
        package_require_file "$WINDOWS_VERIFY_CA_FILE"
        authenticode_verify_args=(-CAfile "$WINDOWS_VERIFY_CA_FILE")
    fi
    while IFS= read -r -d '' binary; do
        osslsigncode verify "${authenticode_verify_args[@]}" -in "$binary" >/dev/null 2>&1 \
            || package_die "invalid Authenticode signature: $(basename "$binary")"
    done < <(find "${root}/bin" -maxdepth 1 -type f \
        \( -iname '*.exe' -o -iname '*.dll' \) -print0)
else
    package_die "osslsigncode is required to verify Windows release signatures"
fi

package_verify_release_info "$root" "${root}/release-info.json"
echo "verified ${archive}"
