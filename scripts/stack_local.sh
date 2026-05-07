#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    printf 'Usage: %s [up|down|logs]\n' "$(basename "$0")"
}

command_name="${1:-up}"
case "$command_name" in
    up)
        docker compose -f "$REPO_ROOT/docker-compose.yml" up --build -d
        ;;
    down)
        docker compose -f "$REPO_ROOT/docker-compose.yml" down
        ;;
    logs)
        docker compose -f "$REPO_ROOT/docker-compose.yml" logs -f --tail=200
        ;;
    *)
        usage
        exit 1
        ;;
esac