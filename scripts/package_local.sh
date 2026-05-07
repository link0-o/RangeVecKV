#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/package"
INSTALL_DIR="${REPO_ROOT}/dist/install"

export PATH="$HOME/.local/bin:$PATH"

TOOLCHAIN_ARGS=()
if [[ -n "${VCPKG_ROOT:-}" && -f "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" ]]; then
	TOOLCHAIN_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
elif [[ -f "$HOME/.local/src/vcpkg/scripts/buildsystems/vcpkg.cmake" ]]; then
	TOOLCHAIN_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$HOME/.local/src/vcpkg/scripts/buildsystems/vcpkg.cmake")
fi

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G Ninja -DKVAI_ENABLE_PACKAGING=ON "${TOOLCHAIN_ARGS[@]}"
cmake --build "${BUILD_DIR}" --parallel
rm -rf "${INSTALL_DIR}"
cmake --install "${BUILD_DIR}" --prefix "${INSTALL_DIR}"
cpack --config "${BUILD_DIR}/CPackConfig.cmake"

printf 'Install tree: %s\n' "${INSTALL_DIR}"
printf 'Packages in: %s\n' "${BUILD_DIR}"