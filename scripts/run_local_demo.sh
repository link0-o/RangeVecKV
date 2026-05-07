#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$HOME/.local/bin:$PATH"

SERVER_BINARY="${REPO_ROOT}/build/local/src/gateway/kvai_server"
CONFIG_FILE="${REPO_ROOT}/config/server.yaml"

"${SERVER_BINARY}" --config "${CONFIG_FILE}" --serve &
SERVER_PID=$!

cleanup() {
	if kill -0 "$SERVER_PID" >/dev/null 2>&1; then
		kill "$SERVER_PID" >/dev/null 2>&1 || true
		wait "$SERVER_PID" 2>/dev/null || true
	fi
}

trap cleanup EXIT

for _ in $(seq 1 30); do
	if curl -fsS http://127.0.0.1:8080/healthz >/dev/null 2>&1; then
		break
	fi
	sleep 0.2
done

printf '=== healthz ===\n'
"${REPO_ROOT}/scripts/http_smoke.sh" http://127.0.0.1:8080