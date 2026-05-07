#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$HOME/.local/bin:$PATH"

SERVER_BIN="${REPO_ROOT}/build/local/src/gateway/kvai_server"

if [[ ! -x "${SERVER_BIN}" ]]; then
	printf 'kvai_server not found: %s\n' "${SERVER_BIN}" >&2
	printf 'Run ./scripts/build_local.sh and wait for it to finish successfully before starting the service.\n' >&2
	exit 1
fi

"${SERVER_BIN}" --config "${REPO_ROOT}/config/server.yaml" --serve