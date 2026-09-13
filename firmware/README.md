# AZ3166 Firmware

Component design and reuse guides live alongside the sources. Start with the
[component directory](../README.md#reusable-firmware-components) for capabilities,
embedding examples, dependency and lifetime contracts, and test entry points.
The instructions below cover the shared toolchain and application setup.

## Supported Toolchain

The checked configuration is:

| Component | Version |
| --- | --- |
| Arduino IDE | 1.8.19 |
| Board package | `AZ3166:stm32f4:2.0.2` |
| ArduinoMDNS | 1.1.0 |
| FQBN | `AZ3166:stm32f4:MXCHIP_AZ3166` |
| GNU Arm toolchain | `5_4-2016q3` from the board package |
| OpenOCD | `0.10.0` from the board package |

The board package index is:

```text
https://raw.githubusercontent.com/AaronWangTT/azureiotdevkit_tools/ac5055ec3a8fddd3135b1c367a9f630d33f65ef8/package_azureboard_index.json
```

That immutable index declares the 5,497,641-byte `AZ3166-2.0.2.zip` with SHA-256
`5914d3e7b988fdc50b00ff241b7ac191fd6fb9bdc234b4a3f86c51ed0d5e9677`.
Arduino Board Manager verifies the Core, GNU Arm toolchain, and OpenOCD
archives while installing them. The installer separately verifies ArduinoMDNS
1.1.0 with SHA-256
`f7a4c6f53d614d05aef3c6c02f6f49b4057202a42a8e40f63bdb062e99162e47`.

On Windows, install Arduino IDE 1.8.19 and the pinned Core with:

```powershell
& .\firmware\tools\Install-Az3166Toolchain.ps1
```

The script reuses an existing Arduino IDE from `-ArduinoExecutable`,
`ARDUINO_IDE_PATH`, a standard Program Files installation, or `PATH`. If none is
found, it downloads Arduino IDE 1.8.19 into `.tools\arduino-1.8.19` before
installing `AZ3166:stm32f4:2.0.2`. It installs ArduinoMDNS 1.1.0 under
`.tools\sketchbook\libraries` and all build helpers select that repository-local
sketchbook explicitly.

## Configuration

Application settings, cloud configuration, and the TLS trust anchor live under
`AZ3166/src/config/`, separate from the reusable transport in `AZ3166/src/cloud/`.
Cloud upload is disabled in a clean checkout. Local deployment files are
optional and ignored by Git:

```powershell
Copy-Item firmware/AZ3166/src/config/cloud_deployment.example.h firmware/AZ3166/src/config/cloud_deployment.h
Copy-Item firmware/AZ3166/src/config/cloud_secrets.example.h firmware/AZ3166/src/config/cloud_secrets.h
```

Set the HTTPS endpoint in `cloud_deployment.h` and a random device key of at
least 32 characters in `cloud_secrets.h`. Verify that `cloud_ca.h` contains the
trust anchor for the server's current certificate chain. Never put a key in an
example, test fixture, command line, build log, or issue report.

When migrating an existing checkout, move its ignored `cloud_deployment.h` and
`cloud_secrets.h` from `AZ3166/` into `AZ3166/src/config/` instead of replacing
them with the example values. Only the new private paths are ignored; migrate
or remove any legacy copies before staging changes. The sketch includes `cloud_config.h`
before `AppConfig.h`, preserving deployment overrides and fallback defaults.

Wi-Fi provisioning is owned by the AZ3166 board package and is not stored in
this repository.

## Local Discovery

After Wi-Fi, IPv4 acquisition, and successful HTTP listener startup, the firmware
advertises this local name:

```text
http://az3166.local/api/telemetry
```

It also publishes an `_http._tcp.local.` service on port 80 with the TXT entry
`path=/api/telemetry`. This application configures those names and records in
`AppConfig`; the reusable discovery component receives them from its caller.
The initial version assumes one `az3166` device per LAN, with no runtime alias
setting or automatic collision renaming. The cloud
device ID, telemetry JSON, and direct-IP HTTP endpoint are unchanged.

Clients must support IPv4 mDNS, and the LAN must permit UDP multicast to
`224.0.0.251:5353`. DNS-only resolvers, multicast isolation, and some VPN or
managed DNS policies can prevent `.local` resolution even when direct-IP access
works. The feature does not require router DNS registration, Internet access,
NTP synchronization, or cloud credentials.

HTTP and mDNS run in separate workers and follow the cached address on reconnect
or DHCP changes. The HTTP worker coordinates advertisement only after its
listener is ready, and withdraws it when the listener stops. Cloud uploads stay
in the main loop but no longer prevent HTTP polling. Shared sensor acquisition
is protected by a short mutex; network operations never hold that sensor lock.
ArduinoMDNS 1.1.0 supplies the responder protocol and lifecycle API. The
firmware keeps only its bounded raw-lwIP transport adapter; dependency source,
release provenance, and LGPL terms are described in the repository's
third-party notices.

## Reusing the HTTP Service

Implement `LocalHttpHandler` to receive a complete request line and a bounded
response buffer. Return the HTTP status, content type, and exact body byte count.
For example, this handler serves a small plain-text response without any sensor
or cloud dependencies:

```cpp
#include <string.h>
#include "src/http/LocalWebServer.h"

class ExampleHandler : public LocalHttpHandler {
public:
    LocalHttpResponse handle(const char *, char *body, size_t capacity) override {
        if (capacity < 3) {
            return {"500 Internal Server Error", "text/plain", 0};
        }
        memcpy(body, "ok\n", 3);
        return {"200 OK", "text/plain", 3};
    }
};

ExampleHandler handler;
LocalWebServer http(handler, 8080, 5000);
```

After application initialization, publish your connectivity snapshot from the
main loop with `http.update(wifiConnected, ipv4Address)`. The address is a
high-octet-first `uint32_t` (`192.0.2.1` is `0xC0000201`), or zero when unavailable.
`http.state()` returns a synchronized snapshot of worker startup, actual listener
readiness, bound address, and the last lifecycle error. Calling
`http.update(false, 0)` requests shutdown of the listener; the worker remains
available for a later reconnect. Destroying the server joins its worker.

To substitute the clock and socket implementation, derive from
`LocalWebServerOperations` and pass the implementation by reference to the
injected constructor. The server borrows this backend rather than copying or
owning it, so it must outlive the server and its worker shutdown. Keep socket
operations nonblocking and synchronize shared backend state: `currentTime()`
can run on the main loop as well as the HTTP worker. The default constructor
continues to use the internal process-lifetime AZ3166 backend.

HTTP sockets are owned by move-only `LocalHttpSocket` objects. A successful
`openListener()` or `acceptClient()` transfers its nonnegative descriptor to the
caller; a failure must release any partially acquired socket before returning.
The native adapter uses scoped ownership for these failure paths. Owners close
through their original backend on reset or destruction, and moves retain that
backend binding. Keep the backend alive until all its socket owners are gone;
its `closeSocket()` implementation must be bounded and nonthrowing.

For discovery, pass an optional `LocalHttpServiceUpdate` to the constructor.
This is a typed `mbed::Callback<void(bool, uint32_t)>`; bind discovery directly
with `mbed::callback(&localDiscovery, &LocalDiscovery::update)`, without a wrapper
function or a separate context pointer. The server stores a copy of the callback
and invokes it on the HTTP worker. An empty callback disables notifications.
Supply a `LocalDiscoveryService`
descriptor containing the hostname (without `.local`), instance/service name
(such as `example._http`), matching listener port, and DNS-SD length-prefixed TXT
data. The application sketch demonstrates the complete wiring.

`LocalDiscovery` is noncopyable and nonmovable. It owns an exclusive active
session on its borrowed `LocalDiscoveryOperations` backend and stops that
session on destruction. Failed startup releases partial state and the lease;
explicit shutdown is not repeated at destruction. A second controller sharing
the same backend waits for its normal retry interval without restarting or
stopping the current owner's service. The backend must outlive all its
controllers, and a bound discovery controller must outlive the HTTP server's
shutdown and join. The default backend still provides one process-lifetime mDNS
responder and worker; stopping a session leaves the worker idle for reuse.

- Initialize handlers and sensors before the first connected update. The callback
  does not own its bound object: the handler, discovery instance or other callback
  target, and borrowed metadata strings must outlive the server's worker.
- Handlers and lifecycle callbacks must be bounded and must not destroy the
  server from its own worker. Synchronize any application state they share with
  the main loop; the telemetry handler delegates sensor synchronization to
  `TelemetryService`.
- One HTTP worker owns one listener and handles one client at a time. It uses a
  6144-byte RTOS stack allocated at startup, a 96-byte request-line buffer, a
  2048-byte total header limit, and a 512-byte response-body buffer. Header reads
  and response writes each have a two-second deadline. Status and content-type
  strings must remain valid until the response is sent.
- This is a small request-line handler API, not a full HTTP framework: request
  bodies, persistent connections, and WebSockets are not supported. Discovery's
  current platform backend supports one advertised service per device.
- Cloud uploads, NTP, and Wi-Fi maintenance remain synchronous in the main loop.
  The watchdog still monitors that loop; threads do not remove Wi-Fi bandwidth
  limits or make an arbitrary blocking handler safe.

## Build and Test

Production components are grouped by capability under `AZ3166/src/`, with
headers beside implementations. Sketches include explicit paths such as
`src/http/LocalWebServer.h`; cross-component source includes are relative to
their owning folder. Test runners select individual dependencies and preserve
the same paths under each staged sketch's `src/` tree. See
[../docs/firmware-design.md](../docs/firmware-design.md#4-source-organization)
for the folder map.

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

For an HTTP-service change, run its focused suite on the board. Close any
serial monitor first; the harness restores production after the suite:

```powershell
& .\firmware\tests\run-local-web-server-tests.ps1 -Action Run -Port COM3
```

The discovery adapter has its own focused suite in
[tests/run-local-discovery-tests.ps1](tests/run-local-discovery-tests.ps1).

Run all suites on a connected board and restore production firmware after each
suite only when a full hardware regression is required:

```powershell
& .\firmware\tests\run-all-tests.ps1 -Action Run -Port COM3
```

An upload is successful only when the command exits zero and OpenOCD reports
`Verified OK`. The AZ3166 2.0.2 build can emit a repeated four-byte `.bss`
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