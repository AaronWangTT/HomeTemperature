#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_DIR=$(cd -- "${SCRIPT_DIR}/.." && pwd)
cd "${PROJECT_DIR}"

if [[ ! -f .env ]]; then
    echo "Missing ${PROJECT_DIR}/.env. Copy .env.example and set real secrets." >&2
    exit 1
fi

if grep -Eq 'replace-with|you@example\.com|telemetry\.example\.com' .env; then
    echo ".env still contains placeholder values." >&2
    exit 1
fi

sudo docker compose config --quiet
sudo docker run --rm \
    -e ACME_EMAIL="$(sed -n 's/^ACME_EMAIL=//p' .env)" \
    -e PUBLIC_HOST="$(sed -n 's/^PUBLIC_HOST=//p' .env)" \
    -v "${PROJECT_DIR}/Caddyfile:/etc/caddy/Caddyfile:ro" \
    caddy:2.10.2-alpine \
    caddy validate --config /etc/caddy/Caddyfile

sudo docker compose build --pull
sudo docker compose up -d --remove-orphans
sudo docker compose ps

echo
echo "Deployment started. Follow certificate and application logs with:"
echo "  sudo docker compose logs -f --tail=100"