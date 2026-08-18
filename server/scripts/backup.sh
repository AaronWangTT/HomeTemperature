#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
backup_dir="${1:-${HOME}/az3166-gateway-backups}"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
container_backup="/data/telemetry-backup.db"
host_backup="${backup_dir}/telemetry-${timestamp}.db"

cd "${project_dir}"
mkdir -p "${backup_dir}"
chmod 700 "${backup_dir}"

container_id="$(sudo docker compose ps -q api)"
if [[ -z "${container_id}" ]]; then
    echo "The api container is not running." >&2
    exit 1
fi

sudo docker compose exec -T api python - <<'PY'
import sqlite3

source = sqlite3.connect("/data/telemetry.db")
destination = sqlite3.connect("/data/telemetry-backup.db")
try:
    source.backup(destination)
finally:
    destination.close()
    source.close()
PY

sudo docker cp "${container_id}:${container_backup}" "${host_backup}"
sudo docker compose exec -T api rm -f "${container_backup}"
sudo chown "$(id -u):$(id -g)" "${host_backup}"
chmod 600 "${host_backup}"

echo "Backup created: ${host_backup}"