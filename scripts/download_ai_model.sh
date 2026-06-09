#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL_DIR="${1:-${ROOT_DIR}/models/chinese-clip-vit-base-patch16}"
REVISION="adcb50f74783c9f8112b4c1f531d806e946ffbc0"
BASE_URL="https://huggingface.co/Xenova/chinese-clip-vit-base-patch16/resolve/${REVISION}"
MODEL_SHA256="a2c2849329eafb24100acec5a9421a39f8cb445296444c5c98855b69e5e7a751"
VOCAB_SHA256="45bbac6b341c319adc98a532532882e91a9cefc0329aa57bac9ae761c27b291c"
TOKENIZER_SHA256="7dfbf1966ebf99d471c3796e9b457329d2b2182b817e144f1e904b957745c839"
CONFIG_SHA256="19447ad8c20d274f0644a6663af56286be98bd2d0e5f9472fcb318e04fcd6961"
PREPROCESSOR_SHA256="61a78fdd2c7ac17b54b6190c0f4cb23423192c535003d52528d01e318a47608b"

mkdir -p "${MODEL_DIR}"

download() {
    local url="$1"
    local destination="$2"
    local expected_sha256="$3"
    local temporary="${destination}.part"

    if [[ -f "${destination}" ]] && echo "${expected_sha256}  ${destination}" | sha256sum --check --status; then
        echo "Keeping verified $(basename "${destination}")."
        return
    fi

    echo "Downloading $(basename "${destination}")..."
    curl -fL --retry 3 --retry-delay 2 -o "${temporary}" "${url}"
    echo "${expected_sha256}  ${temporary}" | sha256sum --check --status
    mv "${temporary}" "${destination}"
}

download "${BASE_URL}/onnx/model_quantized.onnx?download=true" "${MODEL_DIR}/model_quantized.onnx" "${MODEL_SHA256}"
download "${BASE_URL}/vocab.txt?download=true" "${MODEL_DIR}/vocab.txt" "${VOCAB_SHA256}"
download "${BASE_URL}/tokenizer.json?download=true" "${MODEL_DIR}/tokenizer.json" "${TOKENIZER_SHA256}"
download "${BASE_URL}/config.json?download=true" "${MODEL_DIR}/config.json" "${CONFIG_SHA256}"
download "${BASE_URL}/preprocessor_config.json?download=true" "${MODEL_DIR}/preprocessor_config.json" "${PREPROCESSOR_SHA256}"

echo "AI model is ready in ${MODEL_DIR}"
