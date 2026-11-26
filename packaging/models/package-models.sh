#!/usr/bin/env bash
# Package the face-recognition models shipped with Smile2Unlock, grouped by
# their upstream source. Defaults to the LightFacePipeline ONNX models; pass
# --seetaface to package the legacy optional SeetaFace6 .csta models.
#
#   packaging/models/package-models.sh [--seetaface] [RELEASE_SUFFIX]
#
# RELEASE_SUFFIX defaults to v1; bump it when the model set changes so the
# dedicated "models" GitHub Release can carry independent versions.
#
# Output (default build/packages/):
#   smile2unlock-models-lightface-<suffix>.zip (+ .sha256)
#   or with --seetaface: smile2unlock-models-seetaface6-<suffix>.zip (+ .sha256)
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source_kind="lightface"
if [[ "${1:-}" == "--seetaface" ]]; then
    source_kind="seetaface"
    shift
fi
suffix="${1:-v1}"
output_dir="${project_dir}/build/packages"
staging_root="${project_dir}/build/package-stage/models"

# shellcheck source=packaging/lib/common.sh
source "${project_dir}/packaging/lib/common.sh"

if [[ "$source_kind" == "seetaface" ]]; then
    model_dir="${project_dir}/assets/models/seeta"
    name="smile2unlock-models-seetaface6-${suffix}"
    staging="${staging_root}/${name}"

    [[ -f "${model_dir}/face_recognizer.csta" ]] || {
        echo "models: SeetaFace model set not found under ${model_dir}" >&2
        exit 1
    }

    rm -rf -- "${staging}"
    mkdir -p -- "${staging}/seeta"
    cp "${model_dir}"/*.csta "${staging}/seeta/"
    cp "${project_dir}/licenses/SeetaFace6-BSD-2-Clause.txt" \
        "${staging}/LICENSE-SeetaFace6-BSD-2-Clause.txt"

    cat > "${staging}/README.md" <<EOF
# Smile2Unlock face models — SeetaFace6 (legacy optional backend)

Face recognition models redistributed from the
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
- Linux (tar/deb/rpm/pacman layout): \`/usr/share/smile2unlock/models/seeta/\`
EOF
    ( cd "${staging}" && sha256sum -- seeta/*.csta > SHA256SUMS )
else
    model_dir="${project_dir}/assets/models/lightface"
    name="smile2unlock-models-lightface-${suffix}"
    staging="${staging_root}/${name}"

    for f in detector_5pt_lcf.onnx recognizer_edgeface.onnx fas_spatial_enc.onnx fas_temporal_head.onnx; do
        [[ -f "${model_dir}/${f}" ]] || {
            echo "models: LightFace model ${f} not found under ${model_dir}" >&2
            exit 1
        }
    done

    rm -rf -- "${staging}"
    mkdir -p -- "${staging}/lightface"
    cp "${model_dir}"/*.onnx "${staging}/lightface/"
    cp "${project_dir}/licenses/ONNXRuntime-MIT.txt" "${staging}/LICENSE-ONNXRuntime-MIT.txt"

    cat > "${staging}/README.md" <<EOF
# Smile2Unlock face models — LightFace (default)

Face detection, alignment, recognition, and temporal anti-spoofing ONNX
models trained in LightFacePipeline for Smile2Unlock.

| File | Role |
| --- | --- |
| detector_5pt_lcf.onnx | face detection + 5-point landmarks |
| recognizer_edgeface.onnx | face embedding (512-d) |
| fas_spatial_enc.onnx | streaming anti-spoofing spatial encoder |
| fas_temporal_head.onnx | streaming anti-spoofing 8-frame temporal head |

## Install

Extract so the \`.onnx\` files sit in the model directory of your Smile2Unlock
installation:

- Windows (installer/ZIP layout): \`<install-root>\assets\models\lightface\`
- Linux (tar/deb/rpm/pacman layout): \`/usr/share/smile2unlock/models/lightface/\`

Download page: https://github.com/Smile2Unlock/local-repo/releases/tag/models-lightface-${suffix}

## Integrity

SHA-256 digests for this archive ship next to the download
(\`<archive>.zip.sha256\`); per-file digests are in \`SHA256SUMS\` inside.
EOF
    ( cd "${staging}" && sha256sum -- lightface/*.onnx > SHA256SUMS )
fi

mkdir -p -- "${output_dir}"
archive="${output_dir}/${name}.zip"
rm -f -- "${archive}"
( cd "${staging_root}" && zip -q -r "${archive}" "${name}" )

( cd "${output_dir}" && sha256sum "$(basename "${archive}")" \
    > "$(basename "${archive}").sha256" )

echo "created ${archive}"
