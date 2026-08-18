#!/usr/bin/env bash
set -euo pipefail

image="${1:?Usage: $0 IMAGE}"
container_name="home-temperature-integration-${RANDOM}-${RANDOM}"
device_key="integration-device-key"
dashboard_username="integration-viewer"
dashboard_password="integration-dashboard-password"

cleanup() {
    status=$?
    if ((status != 0)); then
        docker logs "$container_name" || true
    fi
    docker rm --force "$container_name" >/dev/null 2>&1 || true
    exit "$status"
}
trap cleanup EXIT

docker run \
    --detach \
    --name "$container_name" \
    --publish 127.0.0.1::8000 \
    --env "DEVICE_API_KEY=$device_key" \
    --env "DASHBOARD_USERNAME=$dashboard_username" \
    --env "DASHBOARD_PASSWORD=$dashboard_password" \
    "$image" >/dev/null

port="$(docker port "$container_name" 8000/tcp | awk -F: 'NR == 1 { print $NF }')"
base_url="http://127.0.0.1:$port"

for attempt in {1..20}; do
    if curl --fail --silent --show-error "$base_url/healthz" >/dev/null 2>&1; then
        break
    fi
    if ((attempt == 20)); then
        echo "Container did not become healthy after $attempt attempts." >&2
        exit 1
    fi
    sleep 1
done

ingested="$(curl \
    --fail-with-body \
    --silent \
    --show-error \
    --request POST \
    --header "Content-Type: application/json" \
    --header "X-Device-Key: $device_key" \
    --data '{"deviceId":"az3166-integration","temperature":26.5,"humidity":58.3,"pressure":1008.4}' \
    "$base_url/api/v1/telemetry")"
printf '%s' "$ingested" | python -c '
import json
import sys

record = json.load(sys.stdin)
assert record["deviceId"] == "az3166-integration"
assert record["temperature"] == 26.5
'

latest="$(curl \
    --fail-with-body \
    --silent \
    --show-error \
    --user "$dashboard_username:$dashboard_password" \
    "$base_url/api/v1/telemetry/latest?deviceId=az3166-integration")"
printf '%s' "$latest" | python -c '
import json
import sys

record = json.load(sys.stdin)
assert record["deviceId"] == "az3166-integration"
assert record["humidity"] == 58.3
assert record["pressure"] == 1008.4
'
