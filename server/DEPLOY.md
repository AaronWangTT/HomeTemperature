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
chmod +x scripts/*.sh
./scripts/bootstrap-ubuntu.sh
```

The script uses Docker's official apt repository and installs Docker Engine,
Buildx, and the Compose plugin. It deliberately keeps Docker commands behind
`sudo`; adding an account to the `docker` group grants root-equivalent access.

## 4. Transfer the Deployment Project

From Windows PowerShell, package the directory contents and extract them into a
fixed destination. The archive reuses `.dockerignore`, so local secrets, tests,
and runtime files are not uploaded.

```powershell
$source = (Resolve-Path '.\server').Path
$archive = "$env:TEMP\az3166-gateway.tar.gz"
tar -czf $archive -X "$source\.dockerignore" -C $source .
scp $archive <ssh-user>@telemetry.example.com:~/
ssh <ssh-user>@telemetry.example.com "mkdir -p ~/az3166-gateway && tar -xzf ~/az3166-gateway.tar.gz -C ~/az3166-gateway && rm ~/az3166-gateway.tar.gz"
Remove-Item $archive
```

Extracting the archive contents, rather than the `server` directory, avoids an
extra `~/az3166-gateway/server` level. Repeating the command leaves the existing
VM-side `.env` in place.

Then connect:

```powershell
ssh <ssh-user>@telemetry.example.com
```

On the VM:

```bash
cd ~/az3166-gateway
chmod +x scripts/*.sh
```

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
./scripts/deploy.sh
sudo docker compose logs -f --tail=100
```

The deployment script performs these checks before starting:

- Compose configuration is valid.
- Caddy accepts the configuration.
- Required secrets are present and placeholders are gone.
- Current container images are pulled and the API image builds successfully.

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

Update after transferring new source files:

```bash
./scripts/deploy.sh
```

Create a consistent SQLite backup:

```bash
./scripts/backup.sh
```

Keep backups outside the host, for example in encrypted object storage. In the
Azure example, Azure Storage is one suitable destination. A VM disk snapshot
is not a substitute for regular application-level backups.

Restore procedure:

1. Stop the stack with `sudo docker compose down`.
2. Start only the API container with `sudo docker compose up -d api`.
3. Find it with `sudo docker compose ps -q api`.
4. Copy the selected backup to `/data/telemetry.db` with `sudo docker cp`.
5. Start all services with `sudo docker compose up -d`.

## 10. Capacity and Upgrade Path

At one record per minute, one device sends 525,600 records per year. SQLite in
WAL mode is appropriate for this workload. Upgrade to PostgreSQL when there are
many devices, concurrent writers, complex analytics, or retention jobs. Keep
the API contract stable so that database replacement does not require a
firmware change.