#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_DIR=$(cd -- "${SCRIPT_DIR}/.." && pwd)
cd "${PROJECT_DIR}"

if [[ ! -f .env ]]; then
    echo "Missing ${PROJECT_DIR}/.env. Copy .env.example and set real secrets." >&2
    exit 1
fi

env_mode=$(stat -c '%a' .env)
if [[ "${env_mode}" != "600" ]]; then
    echo ".env must have mode 600; current mode is ${env_mode}." >&2
    exit 1
fi

if grep -q $'\r' .env; then
    echo ".env must use LF line endings; carriage returns are not allowed." >&2
    exit 1
fi

required_variables=(
    ACME_EMAIL
    PUBLIC_HOST
    DEVICE_API_KEY
    DASHBOARD_USERNAME
    DASHBOARD_PASSWORD
)

for variable in "${required_variables[@]}"; do
    count=$(grep -c "^${variable}=" .env || true)
    if [[ "${count}" -ne 1 ]]; then
        echo ".env must contain exactly one ${variable} entry." >&2
        exit 1
    fi

    value=$(sed -n "s/^${variable}=//p" .env)
    if [[ -z "${value}" ]]; then
        echo ".env contains an empty ${variable} value." >&2
        exit 1
    fi
done

if grep -Eq 'replace-with|you@example\.com|telemetry\.example\.com' .env; then
    echo ".env still contains placeholder values." >&2
    exit 1
fi

release_revision="${RELEASE_REVISION:-}"
if [[ ! "${release_revision}" =~ ^[0-9a-f]{40}$ ]]; then
    echo "RELEASE_REVISION must be a full 40-character lowercase Git SHA." >&2
    exit 1
fi

deploy_wait_timeout="${DEPLOY_WAIT_TIMEOUT_SECONDS:-120}"
if [[ ! "${deploy_wait_timeout}" =~ ^[1-9][0-9]*$ ]]; then
    echo "DEPLOY_WAIT_TIMEOUT_SECONDS must be a positive integer." >&2
    exit 1
fi

compose_up_help=$(sudo docker compose up --help)
if ! grep -Eq -- '(^|[^[:alnum:]_-])--wait([^[:alnum:]_-]|$)' <<<"${compose_up_help}" ||
    ! grep -Eq -- '(^|[^[:alnum:]_-])--wait-timeout([^[:alnum:]_-]|$)' <<<"${compose_up_help}"; then
    echo "Docker Compose must support --wait and --wait-timeout." >&2
    exit 1
fi

sudo docker compose config --quiet
sudo docker compose pull caddy
sudo docker run --rm \
    -e ACME_EMAIL="$(sed -n 's/^ACME_EMAIL=//p' .env)" \
    -e PUBLIC_HOST="$(sed -n 's/^PUBLIC_HOST=//p' .env)" \
    -v "${PROJECT_DIR}/Caddyfile:/etc/caddy/Caddyfile:ro" \
    caddy:2.10.2-alpine \
    caddy validate --config /etc/caddy/Caddyfile

sudo docker compose build --pull --build-arg "VCS_REF=${release_revision}"
sudo docker compose up \
    -d \
    --remove-orphans \
    --wait \
    --wait-timeout "${deploy_wait_timeout}"
sudo docker compose ps

echo
echo "Deployment completed and services are healthy. Follow logs with:"
echo "  sudo docker compose logs -f --tail=100"