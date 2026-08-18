#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SERVER_DIR=$(cd -- "${SCRIPT_DIR}/.." && pwd)
FIXTURE=$(mktemp -d)
trap 'rm -rf -- "${FIXTURE}"' EXIT

PROJECT_DIR="${FIXTURE}/server"
FAKE_BIN="${FIXTURE}/bin"
SUDO_LOG="${FIXTURE}/sudo.log"
OUTPUT="${FIXTURE}/output.log"
VALID_REVISION=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa

mkdir -p "${PROJECT_DIR}/scripts" "${FAKE_BIN}"
cp "${SERVER_DIR}/scripts/deploy.sh" "${PROJECT_DIR}/scripts/deploy.sh"

cat > "${FAKE_BIN}/stat" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "${FAKE_ENV_MODE:?}"
EOF

cat > "${FAKE_BIN}/sudo" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "${FAKE_SUDO_LOG:?}"
if [[ "$*" == "docker compose up --help" ]]; then
    case "${FAKE_COMPOSE_WAIT_SUPPORT:-yes}" in
        yes)
            printf '%s\n' '      --wait' '      --wait-timeout SECONDS'
            ;;
        timeout-only)
            printf '%s\n' '      --wait-timeout SECONDS'
            ;;
        *)
            printf '%s\n' 'Usage: docker compose up'
            ;;
    esac
fi
EOF

chmod +x "${FAKE_BIN}/stat" "${FAKE_BIN}/sudo"

write_environment() {
    local device_key=${1-test-device-key-000000000000000000000000}
    local duplicate_host=${2:-no}

    cat > "${PROJECT_DIR}/.env" <<EOF
ACME_EMAIL=ci@invalid.test
PUBLIC_HOST=gateway.invalid.test
DEVICE_API_KEY=${device_key}
DASHBOARD_USERNAME=test-viewer
DASHBOARD_PASSWORD=test-dashboard-password-000000000000
EOF
    if [[ "${duplicate_host}" == "yes" ]]; then
        printf '%s\n' 'PUBLIC_HOST=duplicate.invalid.test' >> "${PROJECT_DIR}/.env"
    fi
}

run_deploy() {
    local env_mode=$1
    local revision=$2
    local timeout=$3
    local compose_wait_support=$4

    : > "${SUDO_LOG}"
    (
        cd "${PROJECT_DIR}"
        PATH="${FAKE_BIN}:${PATH}" \
        FAKE_ENV_MODE="${env_mode}" \
        FAKE_SUDO_LOG="${SUDO_LOG}" \
        FAKE_COMPOSE_WAIT_SUPPORT="${compose_wait_support}" \
        RELEASE_REVISION="${revision}" \
        DEPLOY_WAIT_TIMEOUT_SECONDS="${timeout}" \
            bash scripts/deploy.sh
    ) > "${OUTPUT}" 2>&1
}

expect_failure() {
    local name=$1
    local expected_message=$2
    shift 2

    if run_deploy "$@"; then
        printf 'Expected failure: %s\n' "${name}" >&2
        exit 1
    fi
    if ! grep -Fq -- "${expected_message}" "${OUTPUT}"; then
        printf 'Missing expected error for %s\n' "${name}" >&2
        exit 1
    fi
    if grep -Fq -- 'docker compose build' "${SUDO_LOG}"; then
        printf 'Build started after failed preflight: %s\n' "${name}" >&2
        exit 1
    fi
}

write_environment
run_deploy 600 "${VALID_REVISION}" 30 yes
grep -Fq -- 'docker compose pull caddy' "${SUDO_LOG}"
grep -Fq -- "docker compose build --pull --build-arg VCS_REF=${VALID_REVISION}" "${SUDO_LOG}"
grep -Fq -- 'docker compose up -d --remove-orphans --wait --wait-timeout 30' "${SUDO_LOG}"

rm "${PROJECT_DIR}/.env"
expect_failure missing-environment 'Missing ' 600 "${VALID_REVISION}" 30 yes

write_environment
expect_failure environment-mode '.env must have mode 600' 644 "${VALID_REVISION}" 30 yes

write_environment test-device-key-000000000000000000000000 yes
expect_failure duplicate-variable 'exactly one PUBLIC_HOST entry' 600 "${VALID_REVISION}" 30 yes

write_environment ''
expect_failure empty-variable 'empty DEVICE_API_KEY value' 600 "${VALID_REVISION}" 30 yes

write_environment
expect_failure missing-revision 'RELEASE_REVISION must be' 600 '' 30 yes
expect_failure invalid-revision 'RELEASE_REVISION must be' 600 aaaa 30 yes
expect_failure invalid-timeout 'DEPLOY_WAIT_TIMEOUT_SECONDS must be' 600 "${VALID_REVISION}" 0 yes
expect_failure unsupported-compose 'Docker Compose must support' 600 "${VALID_REVISION}" 30 no
expect_failure timeout-only-compose 'Docker Compose must support' 600 "${VALID_REVISION}" 30 timeout-only

printf 'Deploy preflight tests passed.\n'