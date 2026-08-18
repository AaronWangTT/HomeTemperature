# AZ3166 Telemetry Gateway Deployment (Azure VM Example)

This runbook is a reference deployment using an Azure Ubuntu VM. The
application itself is not tied to Azure: it requires a maintained Linux host,
Docker Engine with the Compose plugin, a public DNS name, and inbound TCP 443.
When using another hosting provider or an on-premises server, replace the Azure
snapshot, recovery-console, and network-security steps with equivalent controls.

The example deploys the telemetry API and dashboard at:

```text
https://telemetry.example.com
```

Replace `telemetry.example.com` and `<ssh-user>` with deployment-specific
values. The public service is Caddy on TCP 443 only. FastAPI is reachable only
on the private Docker network. Telemetry is stored in a persistent SQLite
volume.

## 1. Fix the Operating System Baseline

Use an Ubuntu 22.04 or 24.04 LTS host that is receiving security updates.
Confirm the selected release remains supported before deployment.

For this Azure example, a fresh Ubuntu Server 24.04 LTS VM provides a longer
standard-support window and a straightforward rollback path:

1. Take an Azure disk snapshot of the existing VM.
2. Create a new Ubuntu 24.04 LTS VM in the same region and virtual network.
3. Reuse or move the existing static Public IP resource and update the public
  DNS record.
4. Verify SSH access before removing the old VM.

There is no need to complete another release upgrade before deploying this
service. If an in-place `22.04 -> 24.04` upgrade is scheduled later, create
another snapshot and confirm Azure Serial Console access first:

```bash
sudo apt update
sudo apt full-upgrade
sudo reboot
sudo do-release-upgrade        # 22.04 -> 24.04
sudo reboot
```

Keep the Azure serial console or another recovery path available during an
in-place upgrade. Confirm the final version:

```bash
cat /etc/os-release
uname -a
```

Expected after that optional upgrade: `VERSION_ID="24.04"`.

## 2. Configure Public Network Security

Create the public DNS record before requesting an ACME certificate.

In this Azure example, add these inbound TCP rules to the VM NIC or subnet
Network Security Group. On another platform, create equivalent firewall or
security-group rules:

| Priority | Source | Port | Action | Purpose |
|---:|---|---:|---|---|
| 100 | Your public IP `/32` | 22 | Allow | SSH administration |
| 110 | Internet | 443 | Allow | HTTPS, API, dashboard, and TLS-ALPN ACME challenge |

Do not expose ports 80, 8000, 5432, or any Docker daemon port. Confirm that the
Public IP allocation is static and the configured DNS name resolves to it.
This deployment disables HTTP redirects and the HTTP-01 challenge. Caddy issues
and renews certificates through TLS-ALPN-01 on TCP 443, so clients must use an
explicit `https://` URL.

If UFW is enabled, allow the same public services:

```bash
sudo ufw allow OpenSSH
sudo ufw allow 443/tcp
sudo ufw status verbose
```

The provider firewall or Azure NSG remains the primary public boundary.
Docker-published ports can bypass some UFW forwarding rules, so only Caddy
publishes host ports in this project.

## 3. Install Docker Engine

Transfer this `server` directory to the VM first, or transfer it after Docker is
installed. Run the included bootstrap on Ubuntu 22.04 or 24.04:

```bash
./scripts/bootstrap-ubuntu.sh
```

The script uses Docker's official apt repository and installs Docker Engine,
Buildx, and the Compose plugin. It deliberately keeps Docker commands behind
`sudo`; adding an account to the `docker` group grants root-equivalent access.

## 4. Transfer the Deployment Project

Build the release archive from an immutable Git revision, not from the working
directory. The helper runs `git archive` from the repository root so root
`.gitattributes` rules apply, then audits the archive for required files,
forbidden runtime paths, symbolic links, shell executable modes, and CRLF shell
scripts.

```powershell
if (git status --porcelain) {
  throw "Create releases from a clean checkout."
}

$revision = (git rev-parse HEAD).Trim()
$archiveName = "az3166-gateway-$revision.tar.gz"
$archive = Join-Path $env:TEMP $archiveName
$target = "<ssh-user>@telemetry.example.com"
$release = ./server/tools/New-ServerReleaseArchive.ps1 `
  -Revision $revision `
  -OutputPath $archive

scp $release.Archive "${target}:~/$archiveName"
if ($LASTEXITCODE -ne 0) {
  throw "Release upload failed."
}

$remoteHashOutput = (ssh $target "sha256sum -- '$archiveName'" | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
  throw "Remote checksum command failed."
}
$remoteSha256 = ($remoteHashOutput -split '\s+', 2)[0].ToLowerInvariant()
if ($remoteSha256 -ne $release.Sha256) {
  throw "Release checksum mismatch."
}

Remove-Item $archive
```

Only committed files under `server/` enter the archive. Local `.env`, database,
backup, cache, and untracked files cannot enter it. Check the immutable
revision's CI status before transferring it. The local archive is removed only
after upload and remote checksum verification succeed.

Then connect:

```powershell
ssh <ssh-user>@telemetry.example.com
```

On the VM:

```bash
set -euo pipefail

revision=<full-40-character-git-sha>
project_dir="$HOME/az3166-gateway"
archive="$HOME/az3166-gateway-${revision}.tar.gz"
release_dir=$(mktemp -d "$HOME/az3166-gateway-release.XXXXXX")
previous_dir=""

restore_previous_release() {
  status=$?
  if [[ "$status" -ne 0 && -n "$previous_dir" && -d "$previous_dir" ]]; then
    rm -rf -- "$project_dir"
    mv -- "$previous_dir" "$project_dir"
  fi
  if [[ -n "$release_dir" && -d "$release_dir" ]]; then
    rm -rf -- "$release_dir"
  fi
  exit "$status"
}
trap restore_previous_release EXIT

test "$project_dir" = "$HOME/az3166-gateway"
tar -xzf "$archive" -C "$release_dir" --strip-components=1
test -f "$release_dir/compose.yaml"
test -f "$release_dir/scripts/deploy.sh"

while IFS= read -r -d '' shell_script; do
  test -x "$shell_script"
  IFS= read -r shebang < "$shell_script"
  test "$shebang" = '#!/usr/bin/env bash'
  ! LC_ALL=C grep -q $'\r' "$shell_script"
done < <(find "$release_dir" -type f -name '*.sh' -print0)
rm "$archive"

if [[ -e "$project_dir" ]]; then
  test -d "$project_dir"
  if [[ -f "$project_dir/.env" ]]; then
    cp -p -- "$project_dir/.env" "$release_dir/.env"
  fi
  if [[ -d "$project_dir/backups" ]]; then
    cp -a -- "$project_dir/backups" "$release_dir/backups"
  fi

  previous_dir=$(mktemp -d "$HOME/az3166-gateway-previous.XXXXXX")
  rmdir "$previous_dir"
  mv -- "$project_dir" "$previous_dir"
fi

mv -- "$release_dir" "$project_dir"
release_dir=""
cd "$project_dir"
trap - EXIT
if [[ -n "$previous_dir" ]]; then
  printf 'Previous source retained at: %s\n' "$previous_dir"
fi
```

Shell scripts are executable in Git and retain that mode in the release
archive. A missing executable bit therefore indicates a damaged or unsupported
transfer path; rebuild and transfer the archive instead of repairing it in
place. All copying and validation occur before the fixed project directory
changes. The final sibling-directory rename prevents stale source files; if a
rename fails, the exit trap restores the prior directory. The previous source
directory remains until post-deployment acceptance succeeds; remove it only
after recording its path. For an upgrade, create all rollback assets in
section 9 before running this installation block.

## 5. Create Secrets

Generate independent high-entropy values on the VM:

```bash
cp .env.example .env
DEVICE_API_KEY=$(openssl rand -hex 32)
DASHBOARD_PASSWORD=$(openssl rand -base64 32 | tr -d '\n')
printf 'Device key: %s\nDashboard password: %s\n' \
  "$DEVICE_API_KEY" "$DASHBOARD_PASSWORD"
chmod 600 .env
```

Edit `.env`:

```dotenv
ACME_EMAIL=your-real-email@example.com
PUBLIC_HOST=telemetry.example.com
DEVICE_API_KEY=<generated-device-key>
DASHBOARD_USERNAME=admin
DASHBOARD_PASSWORD=<generated-dashboard-password>
```

When `deviceId` is omitted from a query, the API and dashboard select the device
that reported most recently.

Store the generated values in a password manager. Do not commit `.env` or paste
the secrets into chat. The device key will later be copied into the firmware.

## 6. Deploy

Run:

```bash
RELEASE_REVISION=<full-40-character-git-sha> ./scripts/deploy.sh
sudo docker compose logs -f --tail=100
```

The deployment script performs these checks before starting:

- `.env` has mode `600`.
- Every required `.env` key appears exactly once and has a nonempty value.
- Compose configuration is valid.
- Caddy accepts the configuration.
- Placeholder values are gone.
- `RELEASE_REVISION` is a full lowercase Git commit SHA.
- Docker Compose supports the required `--wait` and `--wait-timeout` options.
- The pinned Caddy tag is freshly pulled and the API image builds successfully.
- Compose reports the API healthy before deployment succeeds.

The API image stores the supplied revision in the
`org.opencontainers.image.revision` OCI label. The deployment script refuses
to build without that immutable release identity.

Caddy obtains and renews a public certificate automatically through the
TLS-ALPN-01 challenge. TCP 443 must reach Caddy directly during issuance and
renewal. Leave the log view with `Ctrl+C`; the containers continue running.

## 7. Verify the Service

On the VM:

```bash
curl --fail --resolve \
  telemetry.example.com:443:127.0.0.1 \
  https://telemetry.example.com/healthz
curl --fail https://telemetry.example.com/healthz
```

Confirm that the running API image identifies the intended release:

```bash
api_container=$(sudo docker compose ps -q api)
sudo docker inspect \
  --format '{{ index .Config.Labels "org.opencontainers.image.revision" }}' \
  "${api_container}"
```

Submit a test record without putting the key into shell history:

```bash
read -rsp 'Device API key: ' DEVICE_API_KEY; echo
curl --fail-with-body \
  -H "X-Device-Key: ${DEVICE_API_KEY}" \
  -H 'Content-Type: application/json' \
  --data '{"deviceId":"az3166-01","temperature":26.5,"humidity":58.3,"pressure":1008.4}' \
  https://telemetry.example.com/api/v1/telemetry
unset DEVICE_API_KEY
```

Query the latest value:

```bash
curl --user admin \
  https://telemetry.example.com/api/v1/telemetry/latest
```

Open the dashboard and enter the same dashboard credentials:

```text
https://telemetry.example.com/dashboard
```

Expected API behavior:

- Missing or invalid `X-Device-Key`: `401`
- Valid telemetry: `201`
- Invalid sensor range: `422`
- Missing dashboard credentials: `401`
- No data for a requested device: `404`

## 8. Connect the AZ3166

The firmware should POST once per chosen interval to:

```text
https://telemetry.example.com/api/v1/telemetry
```

Headers:

```text
Content-Type: application/json
X-Device-Key: <DEVICE_API_KEY>
```

Body:

```json
{
  "deviceId": "az3166-01",
  "temperature": 26.5,
  "humidity": 58.3,
  "pressure": 1008.4
}
```

The AZ3166 SDK requires the current root CA certificate that signs the Caddy
certificate. After Caddy is online, inspect the issued chain from a workstation:

```bash
openssl s_client \
  -connect telemetry.example.com:443 \
  -servername telemetry.example.com \
  -showcerts </dev/null
```

Embed the appropriate root CA PEM in firmware, not the short-lived leaf
certificate. Verify the chain before choosing the root; Caddy's ACME issuer can
change chains over time.

## 9. Operations

View status and logs:

```bash
sudo docker compose ps
sudo docker compose logs --tail=200 api
sudo docker compose logs --tail=200 caddy
```

Create a consistent SQLite backup:

```bash
./scripts/backup.sh
```

Keep backups outside the host, for example in encrypted object storage. In the
Azure example, Azure Storage is one suitable destination. A VM disk snapshot
is not a substitute for regular application-level backups.

### Production Upgrade

Before replacing source files, record five rollback assets on the VM: a
consistent database backup, the current source tree, the current `.env`, a tag
for the running API image, and a protected file containing that image's ID and
configured reference. The following commands do not print secrets or telemetry
rows:

```bash
cd ~/az3166-gateway
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p backups
chmod 700 backups

./scripts/backup.sh "$PWD/backups"
tar \
  --exclude='./.env' \
  --exclude='./backups' \
  -czf "backups/source-pre-${timestamp}.tar.gz" .
cp --preserve=mode,timestamps .env "backups/env-pre-${timestamp}"
chmod 600 "backups/env-pre-${timestamp}"

api_container=$(sudo docker compose ps -q api)
test -n "${api_container}"
old_image_id=$(sudo docker inspect --format '{{.Image}}' "${api_container}")
old_image_reference=$(sudo docker inspect --format '{{.Config.Image}}' "${api_container}")
printf 'image_id=%s\nimage_reference=%s\n' \
  "$old_image_id" "$old_image_reference" \
  > "backups/api-image-pre-${timestamp}.txt"
chmod 600 "backups/api-image-pre-${timestamp}.txt"
sudo docker image tag \
  "$old_image_id" \
  "az3166-gateway-api:rollback-${timestamp}"
```

Compose assigns the API image the stable name
`az3166-gateway-api:latest`, independent of the project directory or
`COMPOSE_PROJECT_NAME`. The timestamped tag and recorded image ID remain
reliable rollback identities, including when the previous release predates
that stable name.

Retain the timestamp and copy the backups to independent storage. Transfer and
extract the audited archive as described in section 4. Preserve `.env`; when a
release adds a required key such as `PUBLIC_HOST`, edit the file on the VM,
keep exactly one nonempty assignment, and restore mode `600`. Never replace
production `.env` with `.env.example`.

Deploy the same revision used to build the archive:

```bash
cd ~/az3166-gateway
RELEASE_REVISION=<full-40-character-git-sha> ./scripts/deploy.sh
```

Complete the checks in section 7. At minimum, verify internal and external
`/healthz`, the image revision label, expected `401` responses without
credentials, a valid authenticated write/read cycle, and a post-deployment
SQLite backup. Confirm that Caddy access logs do not contain the test device
key used for acceptance.

### Application Rollback

Use the matching pre-upgrade timestamp. Preserve failed-release logs before
the rollback. Prepare and validate the restored source in a sibling directory
before stopping Compose. The failure trap restores the failed-release source if
the directory swap or rollback startup fails:

```bash
set -euo pipefail

project_dir="$HOME/az3166-gateway"
test -d "$project_dir"
rollback_image="az3166-gateway-api:rollback-<timestamp>"
expected_image_id=$(sudo docker image inspect \
  --format '{{.Id}}' "$rollback_image")
recorded_image_id=$(sed -n 's/^image_id=//p' \
  "$project_dir/backups/api-image-pre-<timestamp>.txt")
test -n "$recorded_image_id"
test "$expected_image_id" = "$recorded_image_id"

rollback_dir=$(mktemp -d "$HOME/az3166-gateway-rollback.XXXXXX")
failed_dir=""

restore_failed_release() {
  status=$?
  if [[ "$status" -ne 0 && -n "$failed_dir" && -d "$failed_dir" ]]; then
    if [[ -f "$project_dir/compose.yaml" ]]; then
      (
        cd "$project_dir"
        sudo docker compose \
          -f compose.yaml \
          -f "$rollback_override" \
          down
      ) >/dev/null 2>&1 || true
    fi
    rm -rf -- "$project_dir"
    mv -- "$failed_dir" "$project_dir"
  fi
  if [[ -n "$rollback_dir" && -d "$rollback_dir" ]]; then
    rm -rf -- "$rollback_dir"
  fi
  exit "$status"
}
trap restore_failed_release EXIT

tar -xzf "$project_dir/backups/source-pre-<timestamp>.tar.gz" \
  -C "$rollback_dir"
test -f "$rollback_dir/compose.yaml"
test -f "$rollback_dir/scripts/deploy.sh"
cp "$project_dir/backups/env-pre-<timestamp>" "$rollback_dir/.env"
chmod 600 "$rollback_dir/.env"
cp -a "$project_dir/backups" "$rollback_dir/backups"

rollback_override="backups/rollback-<timestamp>.override.yaml"
cat > "$rollback_dir/$rollback_override" <<EOF
services:
  api:
    image: ${rollback_image}
EOF
chmod 600 "$rollback_dir/$rollback_override"

(
  cd "$project_dir"
  sudo docker compose down
)
cd "$HOME"

failed_dir=$(mktemp -d "$HOME/az3166-gateway-failed.XXXXXX")
rmdir "$failed_dir"
mv -- "$project_dir" "$failed_dir"
mv -- "$rollback_dir" "$project_dir"
rollback_dir=""

(
  cd "$project_dir"
  sudo docker compose \
    -f compose.yaml \
    -f "$rollback_override" \
    up \
    -d \
    --remove-orphans \
    --no-build \
    --force-recreate \
    --wait \
    --wait-timeout 120
)

api_container=$(
  cd "$project_dir"
  sudo docker compose \
    -f compose.yaml \
    -f "$rollback_override" \
    ps -q api
)
test -n "$api_container"
actual_image_id=$(sudo docker inspect \
  --format '{{.Image}}' "$api_container")
test "$actual_image_id" = "$expected_image_id"
trap - EXIT
printf 'Failed release retained at: %s\n' "$failed_dir"
cd "$project_dir"
```

The override binds the restored Compose definition directly to the saved image,
including during the first upgrade to a release that defines the stable image
name. Verify health and authentication again. Older images may not contain the
OCI revision label, so exact image-ID equality is the rollback identity check.
Keep the failed-release directory until acceptance succeeds, then remove it
after recording its path. Restore the database only when the failed release
changed or damaged stored data; an application rollback normally reuses the
existing persistent volume.

### Database Restore

Keep the API stopped while replacing SQLite files. If the current database is
readable, create one final backup before stopping the stack. Select the backup
by path without printing its contents. After an application rollback, export
`ROLLBACK_OVERRIDE=backups/rollback-<timestamp>.override.yaml` so every
one-off container uses the exact saved image:

```bash
cd ~/az3166-gateway
backup="$HOME/az3166-gateway-backups/telemetry-<timestamp>.db"
test -f "$backup"
compose_files=(-f compose.yaml)
if [[ -n "${ROLLBACK_OVERRIDE:-}" ]]; then
  test -f "$ROLLBACK_OVERRIDE"
  compose_files+=(-f "$ROLLBACK_OVERRIDE")
fi
sudo docker compose "${compose_files[@]}" down

sudo docker compose "${compose_files[@]}" run \
  --rm \
  --no-deps \
  --no-build \
  -T \
  --entrypoint sh \
  api \
  -c 'umask 077; cat > /data/telemetry.restore.db' < "$backup"

sudo docker compose "${compose_files[@]}" run \
  --rm \
  --no-deps \
  --no-build \
  -T \
  --entrypoint python \
  api - <<'PY'
import sqlite3
from pathlib import Path

staged = Path("/data/telemetry.restore.db")
database = Path("/data/telemetry.db")
database_set = (
  database,
  Path(f"{database}-wal"),
  Path(f"{database}-shm"),
)
previous_set = (
  Path("/data/telemetry.pre-restore.db"),
  Path("/data/telemetry.pre-restore.db-wal"),
  Path("/data/telemetry.pre-restore.db-shm"),
)


def quick_check(path: Path) -> bool:
  connection = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
  try:
    return connection.execute("PRAGMA quick_check").fetchall() == [("ok",)]
  finally:
    connection.close()


if not quick_check(staged):
  raise SystemExit("Backup failed SQLite quick_check.")

staged.chmod(0o600)
for previous in previous_set:
  previous.unlink(missing_ok=True)

try:
  for current, previous in zip(database_set, previous_set, strict=True):
    if current.exists():
      current.replace(previous)

  staged.replace(database)
  if not quick_check(database):
    raise RuntimeError("Restored database failed SQLite quick_check.")
except Exception:
  for current in database_set:
    current.unlink(missing_ok=True)
  for current, previous in zip(database_set, previous_set, strict=True):
    if previous.exists():
      previous.replace(current)
  raise
else:
  for previous in previous_set:
    previous.unlink(missing_ok=True)
PY

sudo docker compose "${compose_files[@]}" up \
  -d \
  --remove-orphans \
  --no-build \
  --wait \
  --wait-timeout 120
```

Both restore containers run as the image's unprivileged application user, so
the restored file keeps the ownership required by the API. The original
database, WAL, and shared-memory files remain a recoverable set until the
restored database passes its second `quick_check`; any exception restores that
entire set. No telemetry rows are printed. Complete the service checks in
section 7 and create a new backup after restoration.

## 10. Capacity and Upgrade Path

At one record per minute, one device sends 525,600 records per year. SQLite in
WAL mode is appropriate for this workload. Upgrade to PostgreSQL when there are
many devices, concurrent writers, complex analytics, or retention jobs. Keep
the API contract stable so that database replacement does not require a
firmware change.