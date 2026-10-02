#!/usr/bin/env bash
set -euo pipefail

# Exercise the real packagers and verifier with tiny ELF fixtures. No application
# build, model download, package installation, or host PAM changes are needed.
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
test_dir="$(mktemp -d)"
trap 'rm -rf -- "$test_dir"' EXIT
fixture_dir="${test_dir}/fixture checkout"
build_dir="${fixture_dir}/build/linux/x86_64/release"
runtime_dir="${test_dir}/runtime libraries"
output_dir="${test_dir}/output packages"
mkdir -p "$build_dir/assets/i18n" "$build_dir/assets/models/seeta" \
    "$runtime_dir" "$fixture_dir/assets/icons" "$fixture_dir/docs"
cp -a "${project_dir}/packaging" "${project_dir}/licenses" \
    "${project_dir}/NOTICE" "${project_dir}/LICENSE" "${project_dir}/version.txt" "$fixture_dir/"
cp "${project_dir}/docs/linux_pam_setup.md" "$fixture_dir/docs/"
cp "${project_dir}/assets/icons/Smile2Unlock.png" "$fixture_dir/assets/icons/"

printf 'int fixture(void) { return 0; }\n' | \
    cc -x c -shared -fPIC -Wl,-soname,libslint_cpp.so.1 -o "$runtime_dir/libslint_cpp.so.1" -
ln -s libslint_cpp.so.1 "$runtime_dir/libslint_cpp.so"
printf 'int detector(void) { return 0; }\n' | \
    cc -x c -shared -fPIC -o "$runtime_dir/libSeetaFaceDetector600.so" -
printf '#include <iostream>\nextern "C" int fixture(void); extern "C" int detector(void); int main() { std::cout << "runtime fixture"; return fixture() + detector(); }\n' | \
    c++ -x c++ - -L"$runtime_dir" -Wl,-rpath,"$runtime_dir" \
        -lslint_cpp -lSeetaFaceDetector600 -o "$build_dir/su_app"
cp "$build_dir/su_app" "$build_dir/su_authd"
printf 'int main(void) { return 0; }\n' | cc -x c - -o "$build_dir/su_deploy_helper"
printf 'int pam_sm_authenticate(void) { return 0; } int pam_sm_setcred(void) { return 0; }\n' | \
    cc -x c -shared -fPIC -o "$build_dir/pam_smile2unlock.so" -
for language in en zh-CN; do
    printf '{}\n' > "$build_dir/assets/i18n/${language}.json"
done
for model in face_detector face_landmarker_pts5 face_recognizer fas_first fas_second; do
    printf 'packaging fixture\n' > "$build_dir/assets/models/seeta/${model}.csta"
done

# mktemp's parent is mode 700: a dropped-privilege makepkg cannot read this
# checkout or output directory directly. The root path must isolate its inputs.
echo "Linux packaging smoke test: uid=$(id -u)"
"${fixture_dir}/packaging/linux/package.sh" --format all --output-dir "$output_dir"
version="$(tr -d '[:space:]' < "$fixture_dir/version.txt")"
archives=("$output_dir"/*)
[[ ${#archives[@]} == 4 ]] || { echo "expected four package formats" >&2; exit 1; }
tar --numeric-owner -tvf "$output_dir/smile2unlock-${version}-1-x86_64.pkg.tar.zst" \
    | awk '$2 != "0/0" { exit 1 }'
[[ "$(stat -c '%u' "$fixture_dir")" == "$(id -u)" ]] \
    || { echo "packaging changed checkout ownership" >&2; exit 1; }
echo "Linux packaging smoke test passed (all formats, archive ownership, runtime symlink)."
