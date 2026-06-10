#!/usr/bin/env bash

set -euo pipefail

BASE_URL="${1:-http://127.0.0.1:8080}"
API_KEY="${KVAI_API_KEY:-}"

AUTH_ARGS=()
if [[ -n "$API_KEY" ]]; then
    AUTH_ARGS=(-H "X-API-Key: ${API_KEY}")
fi

printf '=== healthz ===\n'
health="$(curl -fsS "$BASE_URL/healthz")"
printf '%s\n' "$health"
grep -q '"status"' <<<"$health"
grep -q 'vector_index_outbox_state' <<<"$health"
grep -q 'cluster_slot_count' <<<"$health"

printf '\n=== router ===\n'
router="$(curl -fsS "${AUTH_ARGS[@]}" "$BASE_URL/v1/router?collection=documents&key=doc-smoke")"
printf '%s\n' "$router"
grep -q 'slot_id' <<<"$router"

printf '\n=== document upsert ===\n'
document_payload='{"collection":"documents","key":"doc-smoke","title":"Docker Smoke Test","body":"gateway docker smoke verifies vector index outbox and search","metadata":{"domain":"ci","kind":"smoke"},"mutation_id":"docker-smoke-mutation","version":1}'
upsert="$(curl -fsS "${AUTH_ARGS[@]}" -H 'Content-Type: application/json' -X POST "$BASE_URL/v1/documents" -d "$document_payload")"
printf '%s\n' "$upsert"

printf '\n=== search ===\n'
search="$(curl -fsS "${AUTH_ARGS[@]}" "$BASE_URL/v1/search?q=docker+smoke+vector+index&top_k=3&filter.domain=ci")"
printf '%s\n' "$search"
grep -q 'doc-smoke' <<<"$search"

printf '\n=== kv put/get ===\n'
kv_payload='{"collection":"kv","key":"smoke:1","value":"docker smoke kv payload","metadata":{"kind":"smoke"},"mutation_id":"docker-smoke-kv","version":1}'
curl -fsS "${AUTH_ARGS[@]}" -H 'Content-Type: application/json' -X POST "$BASE_URL/v1/kv" -d "$kv_payload"
printf '\n'
kv_get="$(curl -fsS "${AUTH_ARGS[@]}" "$BASE_URL/v1/kv?collection=kv&key=smoke:1")"
printf '%s\n' "$kv_get"
grep -q 'docker smoke kv payload' <<<"$kv_get"

printf '\n=== metrics ===\n'
metrics="$(curl -fsS "${AUTH_ARGS[@]}" "$BASE_URL/metrics")"
printf '%s\n' "$metrics" | sed -n '1,12p'
grep -q 'kvai_http_requests_total' <<<"$metrics"
