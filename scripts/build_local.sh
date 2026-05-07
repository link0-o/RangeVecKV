#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$HOME/.local/bin:$PATH"

TOOLCHAIN_ARGS=()
if [[ "${KVAI_USE_VCPKG_TOOLCHAIN:-0}" == "1" ]]; then
	if [[ -n "${VCPKG_ROOT:-}" && -f "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" ]]; then
		TOOLCHAIN_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
	elif [[ -f "$HOME/.local/src/vcpkg/scripts/buildsystems/vcpkg.cmake" ]]; then
		TOOLCHAIN_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$HOME/.local/src/vcpkg/scripts/buildsystems/vcpkg.cmake")
	else
		printf 'KVAI_USE_VCPKG_TOOLCHAIN=1 set, but no vcpkg toolchain file was found.\n' >&2
		exit 1
	fi
else
	printf 'Using fallback-only local build. Set KVAI_USE_VCPKG_TOOLCHAIN=1 to install and link full vcpkg backends.\n'
fi

cmake -S "${REPO_ROOT}" -B "${REPO_ROOT}/build/local" -G Ninja "${TOOLCHAIN_ARGS[@]}"
cmake --build "${REPO_ROOT}/build/local" --parallel
ctest --test-dir "${REPO_ROOT}/build/local" --output-on-failure