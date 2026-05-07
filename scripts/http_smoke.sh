#!/usr/bin/env bash

set -euo pipefail

BASE_URL="${1:-http://127.0.0.1:8080}"
API_KEY="${KVAI_API_KEY:-}"

AUTH_ARGS=()
if [[ -n "$API_KEY" ]]; then
    AUTH_ARGS=(-H "X-API-Key: ${API_KEY}")
fi

printf '=== healthz ===\n'
curl -fsS "$BASE_URL/healthz"

printf '\n=== router ===\n'
curl -fsS "${AUTH_ARGS[@]}" "$BASE_URL/v1/router?collection=documents&key=doc-001"

printf '\n=== search ===\n'
curl -fsS "${AUTH_ARGS[@]}" "$BASE_URL/v1/search?q=gateway+health&top_k=2"

printf '\n=== metrics ===\n'
curl -fsS "${AUTH_ARGS[@]}" "$BASE_URL/metrics" | head -n 12