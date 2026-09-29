#!/usr/bin/env bash
# Package the face-recognition models shipped with Smile2Unlock, grouped by
# their upstream source. Today the tree carries a single source: the
# SeetaFace6 open-source toolkit (seetaface6open), so this produces one
# archive. A future second source gets its own archive alongside.
#
#   packaging/models/package-models.sh [RELEASE_SUFFIX]
#
# RELEASE_SUFFIX defaults to v1; bump it when the model set changes so the
# dedicated "models" GitHub Release can carry independent versions.
#
# Output (default build/packages/):
#   smile2unlock-models-seetaface6-<suffix>.zip
#   smile2unlock-models-seetaface6-<suffix>.zip.sha256
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
suffix="${1:-v1}"
output_dir="${project_dir}/build/packages"
model_dir="${project_dir}/assets/models/seeta"
staging_root="${project_dir}/build/package-stage/models"
name="smile2unlock-models-seetaface6-${suffix}"
staging="${staging_root}/${name}"

# shellcheck source=packaging/lib/common.sh
source "${project_dir}/packaging/lib/common.sh"

[[ -f "${model_dir}/face_recognizer.csta" ]] || {
    echo "models: face model set not found under ${model_dir}" >&2
    exit 1
}

rm -rf -- "${staging}"
mkdir -p -- "${staging}"

cp "${model_dir}"/*.csta "${staging}/"
cp "${project_dir}/licenses/SeetaFace6-BSD-2-Clause.txt" \
    "${staging}/LICENSE-SeetaFace6-BSD-2-Clause.txt"

cat > "${staging}/README.md" <<EOF
# Smile2Unlock face models — SeetaFace6

Face recognition models bundled with Smile2Unlock, redistributed from the
[SeetaFace6 open-source toolkit](https://github.com/SeetaFace6Open)
(seetaface6open) under its BSD-2-Clause license (see the license file in
this archive).

| File | Role |
| --- | --- |
| face_detector.csta | face detection |
| face_landmarker_pts5.csta | 5-point landmarking |
| face_recognizer.csta | face embedding (1024-d) |
| fas_first.csta | anti-spoofing, stage 1 |
| fas_second.csta | anti-spoofing, stage 2 |

## Install

Extract so the \`.csta\` files sit in the model directory of your Smile2Unlock
installation:

- Windows (installer/ZIP layout): \`<install-root>\assets\models\seeta\`
- Linux (tar/deb/rpm/pacman layout): \`/usr/share/smile2unlock/models/\`

The release assets already bundle these models; this archive exists for
offline installs, model-only updates, and redistribution audits.

## Integrity

SHA-256 digests for this archive ship next to the download
(\`<archive>.zip.sha256\`); per-file digests are in \`SHA256SUMS\` inside.
EOF

( cd "${staging}" && sha256sum -- *.csta > SHA256SUMS )

mkdir -p -- "${output_dir}"
archive="${output_dir}/${name}.zip"
rm -f -- "${archive}"
( cd "${staging_root}" && zip -q -r "${archive}" "${name}" )

( cd "${output_dir}" && sha256sum "$(basename "${archive}")" \
    > "$(basename "${archive}").sha256" )

echo "created ${archive}"
