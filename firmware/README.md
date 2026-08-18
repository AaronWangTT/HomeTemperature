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

On Windows, install the pinned Core with:

```powershell
& .\firmware\tools\Install-Az3166Toolchain.ps1 `
  -ArduinoExecutable 'C:\Program Files (x86)\Arduino\arduino_debug.exe'
```

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

## Build and Test

Compile the production sketch without touching the board:

```powershell
& .\firmware\tools\Invoke-Az3166Build.ps1 `
  -Action Verify `
  -Sketch .\firmware\AZ3166\AZ3166.ino
```

Compile the production sketch and all ten test sketches:

```powershell
& .\firmware\tests\run-all-tests.ps1 -Action Verify
```

Run all suites on a connected board and restore production firmware after each
suite:

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