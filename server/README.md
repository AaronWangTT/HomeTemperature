# HomeTemperature Server

The HomeTemperature server accepts authenticated environmental telemetry from
an AZ3166 device, stores it in SQLite, and serves an authenticated dashboard and
query API.

## Architecture

```text
AZ3166 -- HTTPS POST --> Caddy :443 --> FastAPI :8000 --> SQLite
Browser -- HTTPS ------> Caddy :443 --> Dashboard and query API
```

Only Caddy publishes a host port. FastAPI and SQLite remain inside the Compose
network and data volume.

## API

| Method | Path | Authentication | Purpose |
| --- | --- | --- | --- |
| `POST` | `/api/v1/telemetry` | `X-Device-Key` | Validate and store one reading |
| `GET` | `/api/v1/telemetry/latest` | HTTP Basic | Return the latest reading |
| `GET` | `/api/v1/telemetry/history` | HTTP Basic | Return 1-168 hours of history |
| `GET` | `/api/v1/telemetry/count` | HTTP Basic | Count readings in a time window |
| `GET` | `/api/v1/status` | HTTP Basic | Return service and database status |
| `GET` | `/dashboard` | HTTP Basic | Serve the telemetry dashboard |
| `GET` | `/healthz` | Public | Report process liveness |

Telemetry requests use this JSON shape:

```json
{
  "deviceId": "az3166-00112233445566778899AABB",
  "temperature": 26.5,
  "humidity": 58.3,
  "pressure": 1008.4
}
```

The server assigns the UTC receipt timestamp. If a read request omits
`deviceId`, the most recently reporting device is selected.

## Configuration

Production configuration is read from environment variables. Docker Compose
loads them from an ignored `server/.env` file.

| Variable | Required | Default | Purpose |
| --- | --- | --- | --- |
| `PUBLIC_HOST` | Compose | None | Public DNS hostname used by Caddy and ACME |
| `ACME_EMAIL` | Compose | None | Certificate expiry and account contact |
| `DEVICE_API_KEY` | Yes | None | Shared secret accepted from device writers |
| `DASHBOARD_USERNAME` | Yes | None | Dashboard and query API username |
| `DASHBOARD_PASSWORD` | Yes | None | Dashboard and query API password |
| `DATABASE_PATH` | No | `/data/telemetry.db` | SQLite database file |

Create local configuration from the template and replace every placeholder:

```bash
cd server
cp .env.example .env
chmod 600 .env
```

Generate independent high-entropy credentials and keep them in a password
manager. Never commit `.env`, database files, backups, or device keys.

## Development

From the repository root:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r server\requirements-dev.txt
$env:PYTHONPATH = "server"
.\.venv\Scripts\python.exe -m pytest server\tests -q
```

To run the API directly for local development, set the four application
variables and start Uvicorn:

```powershell
$env:PYTHONPATH = "server"
$env:DATABASE_PATH = "$PWD\server\telemetry.db"
$env:DEVICE_API_KEY = "development-device-key-000000000000"
$env:DASHBOARD_USERNAME = "viewer"
$env:DASHBOARD_PASSWORD = "development-dashboard-password"
.\.venv\Scripts\python.exe -m uvicorn app.asgi:app --host 127.0.0.1 --port 8000
```

The direct Uvicorn command is for local development only. Public deployments
must terminate TLS at Caddy.

## Container Deployment

After configuring `server/.env` and public DNS:

```bash
cd server
RELEASE_REVISION=<full-40-character-git-sha> ./scripts/deploy.sh
```

The deployment script requires a full release SHA and `.env` mode `600`,
validates required variables and placeholders, checks Compose health-wait
support, expands the Compose configuration, pulls the pinned Caddy tag,
validates the Caddyfile, records the release revision in the API image, builds
the image, and waits for the stack to become healthy. The public host must
route TCP 443 directly to Caddy so ACME TLS-ALPN-01 certificate issuance and
renewal can succeed. Do not expose port 8000 or the database.

Run the stack on a maintained Linux host with Docker Engine and the Compose
plugin installed. Keep operating-system, firewall, and cloud-network hardening
in infrastructure-specific runbooks rather than this application guide.

See [DEPLOY.md](DEPLOY.md) for a complete Azure Ubuntu VM deployment example.
The application requirements in that runbook also apply to equivalent Linux
hosts on other providers.

## Backup

Create a consistent SQLite backup with the Online Backup API:

```bash
cd server
./scripts/backup.sh
```

Backups default to `~/az3166-gateway-backups/`, outside the repository. Copy
them to independent storage, define a retention policy, and periodically test
an offline restore. Keep the API stopped while replacing the SQLite database;
the runbook provides a staged, integrity-checked restore procedure.

## Security Notes

- Caddy records access logs but removes `X-Device-Key` before encoding them.
- Dashboard credentials use HTTP Basic and therefore require HTTPS.
- One shared writer key currently authorizes any valid submitted `deviceId`.
- `/healthz` checks process liveness, not database readiness.
- Telemetry retention is not automatic.

Review the repository [security policy](../SECURITY.md) and
[server design](../docs/server-design.md) before exposing a deployment to the
Internet.

## Tests

The server tests use temporary SQLite databases and never access production
data:

```powershell
$env:PYTHONPATH = "server"
.\.venv\Scripts\python.exe -m pytest server\tests -q
```

CI additionally validates shell syntax, Compose expansion, the Caddyfile, an
audited Git release archive, the API image revision, and the running API
container.
