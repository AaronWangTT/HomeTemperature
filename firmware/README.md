# AZ3166 Firmware

## Supported Toolchain

The checked configuration is:

| Component | Version |
| --- | --- |
| Arduino IDE | 1.8.19 |
| Board package | `AZ3166:stm32f4:2.0.0` |
| FQBN | `AZ3166:stm32f4:MXCHIP_AZ3166` |
| GNU Arm toolchain | `5_4-2016q3` from the board package |
| OpenOCD | `0.10.0` from the board package |

The board package index is:

```text
https://raw.githubusercontent.com/VSChina/azureiotdevkit_tools/d0c76e57d1ad62610aab0773ba687d55df2e4c91/package_azureboard_index.json
```

That index declares `AZ3166-2.0.0.zip` with MD5
`4f51c0ebf4d510f28c06d203a4ce23f8`. Arduino Board Manager verifies the
archive against the index while installing it.

On Windows, install Arduino IDE 1.8.19 and the pinned Core with:

```powershell
& .\firmware\tools\Install-Az3166Toolchain.ps1
```

The script reuses an existing Arduino IDE from `-ArduinoExecutable`,
`ARDUINO_IDE_PATH`, a standard Program Files installation, or `PATH`. If none is
found, it downloads Arduino IDE 1.8.19 into `.tools\arduino-1.8.19` before
installing `AZ3166:stm32f4:2.0.0`.

## Configuration

Cloud upload is disabled in a clean checkout. Local deployment files are
optional and ignored by Git:

```powershell
Copy-Item firmware/AZ3166/cloud_deployment.example.h firmware/AZ3166/cloud_deployment.h
Copy-Item firmware/AZ3166/cloud_secrets.example.h firmware/AZ3166/cloud_secrets.h
```

Set the HTTPS endpoint in `cloud_deployment.h` and a random device key of at
least 32 characters in `cloud_secrets.h`. Verify that `cloud_ca.h` contains the
trust anchor for the server's current certificate chain. Never put a key in an
example, test fixture, command line, build log, or issue report.

Wi-Fi provisioning is owned by the AZ3166 board package and is not stored in
this repository.

## Local Discovery

After Wi-Fi and IPv4 acquisition, the firmware advertises this fixed local name:

```text
http://az3166.local/api/telemetry
```

It also publishes an `_http._tcp.local.` service on port 80 with the TXT entry
`path=/api/telemetry`. The initial version assumes one `az3166` device per LAN;
there is no configurable alias or automatic collision renaming. The cloud
device ID, telemetry JSON, and direct-IP HTTP endpoint are unchanged.

Clients must support IPv4 mDNS, and the LAN must permit UDP multicast to
`224.0.0.251:5353`. DNS-only resolvers, multicast isolation, and some VPN or
managed DNS policies can prevent `.local` resolution even when direct-IP access
works. The feature does not require router DNS registration, Internet access,
NTP synchronization, or cloud credentials.

The responder follows the cached address on reconnect or DHCP changes. It runs
in a small background worker so synchronous cloud calls do not prevent it from
servicing queries. The vendored ArduinoMDNS 1.0.1 source and its local port are
described in the repository's third-party notices.

## Build and Test

Compile the production sketch without touching the board:

```powershell
& .\firmware\tools\Invoke-Az3166Build.ps1 `
  -Action Verify `
  -Sketch .\firmware\AZ3166\AZ3166.ino
```

Compile the production sketch and all eleven test sketches at an integration
checkpoint:

```powershell
& .\firmware\tests\run-all-tests.ps1 -Action Verify
```

For a discovery change, run only its focused suite on the board. Close any
serial monitor first; the harness restores production after the suite:

```powershell
& .\firmware\tests\run-local-discovery-tests.ps1 -Action Run -Port COM3
```

Run all suites on a connected board and restore production firmware after each
suite only when a full hardware regression is required:

```powershell
& .\firmware\tests\run-all-tests.ps1 -Action Run -Port COM3
```

An upload is successful only when the command exits zero and OpenOCD reports
`Verified OK`. The AZ3166 2.0.0 linker can emit a repeated four-byte `.bss`
alignment warning; resource usage and runtime tests must still be checked after
link-layout changes.

The local smoke test accepts a target address through PowerShell:

```powershell
& .\firmware\tests\test_smoke.ps1 -TargetIp 192.0.2.20
```

## Hardware Boundaries

- Local HTTP is unencrypted and unauthenticated.
- Wi-Fi, NTP, and HTTPS platform calls are synchronous.
- Serial logs can contain device identity, network metadata, and measurements.
- Hardware-in-loop tests are never run automatically by CI.