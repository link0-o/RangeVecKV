#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${SUDO_USER:-}" ]]; then
    TARGET_USER="${SUDO_USER}"
elif [[ -n "${PKEXEC_UID:-}" ]]; then
    TARGET_USER="$(getent passwd "${PKEXEC_UID}" | cut -d: -f1)"
else
    TARGET_USER="${USER}"
fi
TARGET_HOME="$(getent passwd "${TARGET_USER}" | cut -d: -f6)"

if [[ "${EUID}" -eq 0 ]]; then
    SUDO=()
else
    if ! command -v sudo >/dev/null 2>&1; then
        echo "sudo is required to install Docker." >&2
        exit 1
    fi
    SUDO=(sudo)
    sudo -v
fi

if [[ ! -r /etc/os-release ]]; then
    echo "Cannot determine the Linux distribution." >&2
    exit 1
fi

# shellcheck disable=SC1091
. /etc/os-release
if [[ "${ID:-}" != "debian" ]]; then
    echo "This installer currently supports Debian only. Detected: ${ID:-unknown}" >&2
    exit 1
fi

ARCH="$(dpkg --print-architecture)"
CODENAME="${VERSION_CODENAME:-}"
if [[ -z "${CODENAME}" ]]; then
    echo "Cannot determine the Debian codename." >&2
    exit 1
fi

echo "Installing Docker Engine from Docker's official Debian repository..."
"${SUDO[@]}" apt-get update
"${SUDO[@]}" apt-get install -y ca-certificates curl

CONFLICTING_PACKAGES=()
for package in docker.io docker-compose docker-doc podman-docker containerd runc; do
    if dpkg-query -W -f='${Status}' "${package}" 2>/dev/null | grep -q "install ok installed"; then
        CONFLICTING_PACKAGES+=("${package}")
    fi
done
if (( ${#CONFLICTING_PACKAGES[@]} > 0 )); then
    "${SUDO[@]}" apt-get remove -y "${CONFLICTING_PACKAGES[@]}"
fi

"${SUDO[@]}" install -m 0755 -d /etc/apt/keyrings
"${SUDO[@]}" curl -fsSL https://download.docker.com/linux/debian/gpg -o /etc/apt/keyrings/docker.asc
"${SUDO[@]}" chmod a+r /etc/apt/keyrings/docker.asc

printf '%s\n' \
    "Types: deb" \
    "URIs: https://download.docker.com/linux/debian" \
    "Suites: ${CODENAME}" \
    "Components: stable" \
    "Architectures: ${ARCH}" \
    "Signed-By: /etc/apt/keyrings/docker.asc" |
    "${SUDO[@]}" tee /etc/apt/sources.list.d/docker.sources >/dev/null

"${SUDO[@]}" apt-get update
"${SUDO[@]}" apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
"${SUDO[@]}" systemctl enable --now docker
"${SUDO[@]}" usermod -aG docker "${TARGET_USER}"

cd "${ROOT_DIR}"

if [[ ! -f .env ]]; then
    API_KEY="$(tr -d '-' </proc/sys/kernel/random/uuid)$(tr -d '-' </proc/sys/kernel/random/uuid)"
    cat >.env <<EOF
KVAI_API_KEY=${API_KEY}
KVAI_DISCOVERY_BACKEND=etcd
KVAI_ETCD_ENDPOINTS=http://etcd:2379
KVAI_ADVERTISE_HOST=rangeveckv
KVAI_CLUSTER_NODES=node-local@127.0.0.1:8080
KVAI_TLS_MODE=disabled
KVAI_AI_BACKEND=onnxruntime
KVAI_MODEL_PATH=/opt/rangeveckv/models/chinese-clip-vit-base-patch16/model_quantized.onnx
KVAI_TOKENIZER_PATH=/opt/rangeveckv/models/chinese-clip-vit-base-patch16/vocab.txt
DEBIAN_MIRROR=http://mirrors.tuna.tsinghua.edu.cn/debian
DEBIAN_SECURITY_MIRROR=http://mirrors.tuna.tsinghua.edu.cn/debian-security
EOF
    chmod 600 .env
    echo "Created .env with a generated API key."
else
    echo "Keeping existing .env."
fi

if ! grep -q '^KVAI_DISCOVERY_BACKEND=' .env; then
    printf 'KVAI_DISCOVERY_BACKEND=etcd\n' >>.env
fi
if ! grep -q '^KVAI_ETCD_ENDPOINTS=' .env; then
    printf 'KVAI_ETCD_ENDPOINTS=http://etcd:2379\n' >>.env
fi
if ! grep -q '^KVAI_ADVERTISE_HOST=' .env; then
    printf 'KVAI_ADVERTISE_HOST=rangeveckv\n' >>.env
fi
if ! grep -q '^KVAI_AI_BACKEND=' .env; then
    printf 'KVAI_AI_BACKEND=onnxruntime\n' >>.env
fi
if grep -q '^KVAI_MODEL_PATH=/opt/rangeveckv/models/clip.onnx$\|^KVAI_MODEL_PATH=/opt/rangeveckv/models/all-MiniLM-L6-v2/model.onnx$' .env; then
    sed -i 's|^KVAI_MODEL_PATH=.*$|KVAI_MODEL_PATH=/opt/rangeveckv/models/chinese-clip-vit-base-patch16/model_quantized.onnx|' .env
fi
if ! grep -q '^KVAI_TOKENIZER_PATH=' .env; then
    printf 'KVAI_TOKENIZER_PATH=/opt/rangeveckv/models/chinese-clip-vit-base-patch16/vocab.txt\n' >>.env
elif grep -q '^KVAI_TOKENIZER_PATH=/opt/rangeveckv/models/all-MiniLM-L6-v2/vocab.txt$' .env; then
    sed -i 's|^KVAI_TOKENIZER_PATH=.*$|KVAI_TOKENIZER_PATH=/opt/rangeveckv/models/chinese-clip-vit-base-patch16/vocab.txt|' .env
fi
if [[ -d "${TARGET_HOME}/.cache/vcpkg/archives" ]] && ! grep -q '^VCPKG_BINARY_CACHE=' .env; then
    printf 'VCPKG_BINARY_CACHE=%s\n' "${TARGET_HOME}/.cache/vcpkg/archives" >>.env
    echo "Configured the existing vcpkg binary cache for Docker builds."
fi
"${SUDO[@]}" chown "${TARGET_USER}:$(id -gn "${TARGET_USER}")" .env

mkdir -p data images

if [[ ! -f models/chinese-clip-vit-base-patch16/model_quantized.onnx || ! -f models/chinese-clip-vit-base-patch16/vocab.txt ]]; then
    ./scripts/download_ai_model.sh
fi
"${SUDO[@]}" chown -R "${TARGET_USER}:$(id -gn "${TARGET_USER}")" models
"${SUDO[@]}" chown -R "${TARGET_USER}:$(id -gn "${TARGET_USER}")" images

echo "Docker versions:"
"${SUDO[@]}" docker version
"${SUDO[@]}" docker compose version

echo "Building and starting RangeVecKV..."
"${SUDO[@]}" docker compose up --build -d

echo "Waiting for RangeVecKV health check..."
healthy=false
for _ in $(seq 1 90); do
    status="$("${SUDO[@]}" docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' rangeveckv 2>/dev/null || true)"
    if [[ "${status}" == "healthy" ]]; then
        healthy=true
        break
    fi
    if [[ "${status}" == "unhealthy" || "${status}" == "exited" || "${status}" == "dead" ]]; then
        break
    fi
    sleep 2
done

if [[ "${healthy}" != "true" ]]; then
    echo "RangeVecKV did not become healthy. Recent logs:" >&2
    "${SUDO[@]}" docker compose logs --tail=200 rangeveckv >&2
    exit 1
fi

API_KEY="$(sed -n 's/^KVAI_API_KEY=//p' .env | head -n 1)"
KVAI_API_KEY="${API_KEY}" ./scripts/http_smoke.sh http://127.0.0.1:8080

echo
"${SUDO[@]}" docker compose ps
echo
echo "RangeVecKV is running at http://127.0.0.1:8080"
echo "The docker group was added for ${TARGET_USER}; log out and back in before using docker without sudo."
