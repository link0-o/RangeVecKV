#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
USER_NAME="$(id -un)"
USER_HOME="$(getent passwd "${USER_NAME}" | cut -d: -f6)"
LOCAL_BIN="${USER_HOME}/.local/bin"
LOCAL_VENV_ROOT="${USER_HOME}/.local/share/rangeveckv-bootstrap"
LOCAL_VENV_BIN="${LOCAL_VENV_ROOT}/bin"
VCPKG_ROOT_DEFAULT="${USER_HOME}/.local/src/vcpkg"
VCPKG_ROOT="${VCPKG_ROOT:-${VCPKG_ROOT_DEFAULT}}"

APT_PACKAGES=(
    ca-certificates
    git
    curl
    wget
    zip
    unzip
    tar
    xz-utils
    build-essential
    pkg-config
    ninja-build
    make
    protobuf-compiler
    libprotobuf-dev
    autoconf
    automake
    autoconf-archive
    libtool
    bison
    flex
    perl
    python3
    python3-pip
    python3-venv
    gfortran
    nasm
    yasm
    ccache
    gdb
    valgrind
    clang
    clangd
    clang-format
    lld
    lldb
)

log() {
    printf '[bootstrap] %s\n' "$*"
}

append_path_hint() {
    local rc_file="$1"

    if [[ -f "${rc_file}" ]] && grep -Fq 'export PATH="$HOME/.local/bin:$PATH"' "${rc_file}"; then
        return
    fi

    printf '\nexport PATH="$HOME/.local/bin:$PATH"\n' >> "${rc_file}"
}

version_ge() {
    local current="$1"
    local expected="$2"
    printf '%s\n%s\n' "${expected}" "${current}" | sort -V -C
}

ensure_sudo() {
    if ! command -v sudo >/dev/null 2>&1; then
        log 'sudo 不可用，无法自动安装系统工具。'
        exit 1
    fi

    log '即将请求 sudo 权限安装系统工具。'
    sudo -v
}

install_apt_packages() {
    if ! command -v apt-get >/dev/null 2>&1; then
        log '当前脚本只支持 Debian/Ubuntu 的 apt-get。'
        exit 1
    fi

    log '安装系统构建工具链和调试工具。'
    sudo apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${APT_PACKAGES[@]}"
}

install_user_cmake_and_ninja() {
    mkdir -p "${LOCAL_BIN}"
    mkdir -p "${LOCAL_VENV_ROOT}"

    log '通过专用虚拟环境安装用户态 cmake 和 ninja，避免触发系统 Python 的 PEP 668 保护。'
    python3 -m venv "${LOCAL_VENV_ROOT}"
    "${LOCAL_VENV_BIN}/python" -m pip install --upgrade pip
    "${LOCAL_VENV_BIN}/python" -m pip install --upgrade cmake ninja

    ln -sf "${LOCAL_VENV_BIN}/cmake" "${LOCAL_BIN}/cmake"
    ln -sf "${LOCAL_VENV_BIN}/ctest" "${LOCAL_BIN}/ctest"
    ln -sf "${LOCAL_VENV_BIN}/cpack" "${LOCAL_BIN}/cpack"
    ln -sf "${LOCAL_VENV_BIN}/ninja" "${LOCAL_BIN}/ninja"

    export PATH="${LOCAL_BIN}:${PATH}"
    append_path_hint "${USER_HOME}/.bashrc"

    if [[ -f "${USER_HOME}/.zshrc" ]]; then
        append_path_hint "${USER_HOME}/.zshrc"
    fi
}

verify_toolchain() {
    export PATH="${LOCAL_BIN}:${PATH}"

    local cmake_path
    cmake_path="$(command -v cmake || true)"
    if [[ -z "${cmake_path}" ]]; then
        log 'cmake 安装失败。'
        exit 1
    fi

    local cmake_version
    cmake_version="$(cmake --version | head -n1 | awk '{print $3}')"
    if ! version_ge "${cmake_version}" '3.24.0'; then
        log "cmake 版本过低: ${cmake_version}"
        exit 1
    fi

    for tool in gcc g++ make ninja protoc git python3; do
        if ! command -v "${tool}" >/dev/null 2>&1; then
            log "缺少工具: ${tool}"
            exit 1
        fi
    done

    log "cmake: ${cmake_version}"
    log "gcc: $(gcc --version | head -n1)"
    log "g++: $(g++ --version | head -n1)"
    log "ninja: $(ninja --version)"
    log "protoc: $(protoc --version)"
}

bootstrap_vcpkg() {
    mkdir -p "$(dirname "${VCPKG_ROOT}")"

    if [[ ! -d "${VCPKG_ROOT}/.git" ]]; then
        log "克隆 vcpkg 到 ${VCPKG_ROOT}"
        git clone https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}"
    else
        log 'vcpkg 已存在，跳过克隆。'
    fi

    log '引导 vcpkg。'
    "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics
}

print_next_steps() {
    cat <<EOF

环境已准备完成。

当前项目默认建议使用 fallback-only 本地验证路径：
export PATH="${LOCAL_BIN}:\$PATH"
./scripts/build_local.sh
./scripts/run_local_service.sh

另开一个终端执行：
./scripts/http_smoke.sh http://127.0.0.1:8080

如果你要显式启用完整 vcpkg toolchain 和真实后端依赖，再执行：
export VCPKG_ROOT="${VCPKG_ROOT}"
export KVAI_USE_VCPKG_TOOLCHAIN=1
./scripts/build_local.sh

如果你希望后续由我直接执行需要 root 的命令，请先在当前工作区终端手动执行：
sudo -v

如果想让 sudo 会话在较长时间内保持有效，可在你自己的终端里额外执行：
while true; do sudo -n true; sleep 240; done &

EOF
}

main() {
    ensure_sudo
    install_apt_packages
    install_user_cmake_and_ninja
    verify_toolchain
    bootstrap_vcpkg
    print_next_steps
}

main "$@"