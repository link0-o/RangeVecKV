#!/usr/bin/env bash

set -euo pipefail

BASE_URL="${1:-http://127.0.0.1:8080}"
API_KEY="${KVAI_API_KEY:-}"

AUTH_ARGS=()
if [[ -n "$API_KEY" ]]; then
    AUTH_ARGS=(-H "X-API-Key: ${API_KEY}")
fi

curl -fsS "${AUTH_ARGS[@]}" "$BASE_URL/healthz"