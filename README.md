# HomeTemperature

HomeTemperature is an end-to-end environmental telemetry project for the
MXChip AZ3166. The firmware reads the onboard temperature, humidity, and
pressure sensors, serves the latest sample on the local network, and can upload
authenticated telemetry over HTTPS. A small FastAPI, SQLite, and Caddy service
stores the readings and serves an authenticated dashboard.

## Project Preview

| MXChip AZ3166 | Telemetry dashboard |
| :---: | :---: |
| ![MXChip AZ3166 development board](docs/images/az3166_board_view.png) | ![HomeTemperature telemetry dashboard](docs/images/dashboard.png) |
| On-device sensing, identity, connectivity, and upload control | Authenticated latest readings, history, and service state |

Hardware labels and deployment identifiers are redacted in the public images.

## Architecture

```mermaid
flowchart LR
    Operator[Operator browser]
    LocalClient[Trusted-LAN client]

    subgraph Device[AZ3166 device]
        UID[STM32 hardware UID<br/>stable deviceId]
        Sensors[HTS221 and LPS22HB<br/>temperature, humidity, pressure]
        Firmware[Firmware telemetry pipeline<br/>bounded JSON payload]
        Recovery[Wi-Fi and NTP backoff<br/>upload retry and watchdog]
        Serial[Serial diagnostics]

        UID --> Firmware
        Sensors --> Firmware
        Recovery -. protects .-> Firmware
        Firmware -. reports .-> Serial
    end

    LocalClient <-->|Unauthenticated local HTTP| Firmware

    subgraph Host[Linux host / Docker Compose]
        subgraph Edge[Published security boundary]
            Caddy[Caddy on TCP 443<br/>ACME TLS and security headers<br/>16 KiB request limit]
        end

        subgraph Private[Private Compose network]
            API[FastAPI<br/>constant-time authentication<br/>schema and range validation]
            Dashboard[Static dashboard]
        end

        Database[(SQLite WAL<br/>id, device_id, received_at<br/>temperature, humidity, pressure)]
        Volume[(telemetry-data volume)]
        Runtime[Health-gated startup<br/>restart unless-stopped]
        Backup[SQLite Online Backup<br/>off-host copy required]
        Logs[Redacted Caddy access logs<br/>rotating container logs]

        Caddy --> API
        API --> Dashboard
        API -->|Insert and deviceId-filtered queries| Database
        Database --- Volume
        Database -. consistent snapshot .-> Backup
        Runtime -. supervises .-> API
        Runtime -. supervises .-> Caddy
        Caddy -. emits .-> Logs
        API -. emits .-> Logs
    end

    Firmware -->|HTTPS POST<br/>X-Device-Key and deviceId| Caddy
    Operator -->|HTTPS and HTTP Basic| Caddy
```

Key design boundaries:

- **Device identity and authentication:** firmware derives a stable `deviceId`
  from the STM32 UID. Device writes use a shared `X-Device-Key`; dashboard and
  query reads use separate HTTP Basic credentials. Both cross the public
  boundary only over HTTPS.
- **Data model and multi-device isolation:** each row stores `device_id`, the
  server-generated UTC receipt time, temperature, humidity, and pressure. An
  index on `(device_id, received_at)` supports per-device latest, history, and
  count queries. This is logical query isolation, not tenant isolation: the
  shared writer key is not bound to the submitted `deviceId`.
- **Container and security boundary:** only Caddy publishes a host port. The API
  stays on the private Compose network, runs as a non-root user with a read-only
  root filesystem, and persists SQLite data in a named volume. Caddy removes
  `X-Device-Key` before writing access logs.
- **Failure handling:** firmware uses bounded Wi-Fi/NTP retries, classifies
  upload outcomes, and feeds a hardware watchdog. Compose health-gates Caddy on
  API startup and restarts failed containers; SQLite uses WAL transactions and
  the backup script takes consistent snapshots. A host or volume failure still
  requires operator recovery.
- **Observability:** firmware reports state over serial; Docker retains bounded
  API and Caddy logs; `/healthz` reports process liveness and authenticated
  `/api/v1/status` reports uptime and aggregate database state. Metrics, traces,
  alerts, and audit records are intentionally outside the current scope.

## Repository Layout

- `firmware/AZ3166/`: production Arduino sketch and modules
- `firmware/tests/`: compile-time and on-board firmware suites
- `firmware/tools/`: repository-owned AZ3166 build helpers
- `server/`: API, dashboard, containers, and deployment scripts
- `docs/`: implementation design documents

## Firmware Quick Start

The verified Windows toolchain is Arduino IDE 1.8.19 with AZ3166 Core 2.0.0
and board `AZ3166:stm32f4:MXCHIP_AZ3166`.

```powershell
& .\firmware\tools\Install-Az3166Toolchain.ps1

& .\firmware\tools\Invoke-Az3166Build.ps1 `
  -Action Verify `
  -Sketch .\firmware\AZ3166\AZ3166.ino

& .\firmware\tests\run-all-tests.ps1 -Action Verify
```

A clean checkout compiles with cloud upload disabled. To configure a private
deployment, copy the two examples and edit only the ignored local files:

```powershell
Copy-Item firmware/AZ3166/cloud_deployment.example.h firmware/AZ3166/cloud_deployment.h
Copy-Item firmware/AZ3166/cloud_secrets.example.h firmware/AZ3166/cloud_secrets.h
```

See [firmware/README.md](firmware/README.md) for installation, hardware tests,
and credential handling.

## Server Quick Start

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r server/requirements-dev.txt
$env:PYTHONPATH = 'server'
python -m pytest server/tests -q
```

Production deployment uses Docker Compose. Copy `server/.env.example` to
`server/.env`, replace every placeholder, and follow
the [Azure VM deployment example](server/DEPLOY.md). Never commit `server/.env`
or device keys.

## Current Limitations

- The local firmware HTTP endpoint is intentionally unauthenticated and belongs
  only on a trusted LAN.
- One shared device key authenticates all writers; there is no per-device
  revocation, ownership model, or tenant boundary.
- The dashboard follows the most recently reporting device and does not yet
  provide a device registry or selector.
- `/healthz` checks process liveness, not database readiness; the deployment has
  no built-in alerting or high-availability failover.
- Telemetry retention and off-host backup scheduling are operator concerns.

Review [SECURITY.md](SECURITY.md) and the detailed design limitations before
exposing a deployment to the Internet.

## Documentation

- [Firmware design](docs/firmware-design.md)
- [Server design](docs/server-design.md)
- [Azure VM deployment example](server/DEPLOY.md)
- [Contributing](CONTRIBUTING.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)

## License

HomeTemperature is available under the [MIT License](LICENSE). Third-party
components and trust material retain their upstream terms.