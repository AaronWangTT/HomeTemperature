# Platform Support

Keep board identity and watchdog recovery in one place. The maintained AZ3166
Core supplies the process-wide formatting and SDK telemetry behavior relied on
by the application.

## Components

| Component | Responsibility |
| --- | --- |
| [DeviceIdentity.h](DeviceIdentity.h) | Read the STM32 unique identifier and provide a stable application ID. |
| [WatchdogController.h](WatchdogController.h) | Configure the board watchdog, report its reset cause, and feed its timer. |

## DeviceIdentity

`begin()` reads three 32-bit UID words at the STM32 device address and stores a
31-character ID plus its null terminator. Its format is:

```text
az3166-<UID0:8 uppercase hex digits><UID1:8 digits><UID2:8 digits>
```

`get()` returns the object's stored ID after successful initialization, or
`az3166-FFFFFFFFFFFFFFFFFFFFFFFF` before initialization succeeds.
`isInitialized()` distinguishes those cases. The fallback is shared, so do not
use it as a claim of device uniqueness.

```cpp
#include <Arduino.h>
#include "src/platform/DeviceIdentity.h"

DeviceIdentity identity;

void initializeIdentity() {
    if (!identity.begin()) {
        return;
    }
    const char *deviceId = identity.get();
    Serial.println(deviceId);
}
```

Call this after serial initialization. Keep the identity object alive while
other components retain its `get()` pointer. Initialization logs the identifier,
so do not publish unredacted diagnostic logs containing device metadata.

For synthetic IDs in tests, `DeviceIdentity::format(output, capacity, uid0,
uid1, uid2)` formats supplied words without reading hardware. It rejects missing
or undersized output storage. The generated characters are safe for the current
telemetry formatter, which does not escape arbitrary identifier text.

This wrapper has no Wi-Fi, cloud, or sensor dependency. Porting the hardware read
to another MCU requires checking its UID address, size, and desired identity
format. An identifier is not a secret, API key, ownership proof, or authorization
boundary. It also does not determine the application's mDNS hostname.

## WatchdogController

The wrapper records whether the previous restart was watchdog-triggered, then
configures the SDK watchdog using a timeout in milliseconds. `reset()` feeds the
timer only if configuration succeeded; it does not reset the MCU directly.

```cpp
#include <Arduino.h>
#include "src/platform/WatchdogController.h"

WatchdogController watchdog(30000.0f);

void setup() {
    Serial.begin(115200);
    watchdog.begin();
}

void loop() {
    watchdog.reset();
}
```

In a real application, feed between meaningful bounded units of work rather
than using an unrelated heartbeat to conceal lack of progress. The application
currently feeds from the main loop around input, connectivity, and upload work.

- Use one owner for the hardware watchdog. The SDK controls supported timeout
  values and reconfiguration behavior; this wrapper does not provide a disable
  API or independent per-thread timers.
- A blocking operation exceeding the configured timeout can cause a reboot.
  A worker can also become stuck while another thread continues to feed the
  watchdog, so successful feeding is not proof that every service is healthy.
- The wrapper reports configuration success/failure over serial. Its public API
  does not expose that result as a return value from `begin()`.
- Do not deliberately force resets during normal automated test runs or live
  deployments. Recovery checks need a controlled board test with state preserved.

## Board Package Behavior

The firmware requires maintained AZ3166 Core 2.0.2. It carries forward Core
2.0.1's corrected `dtostrf` fractional-digit and width behavior while retaining
rounding and non-finite handling. `TelemetryService` uses that Core function
instead of floating-point `printf`; callers must still provide enough output
storage for the requested width, precision, sign, and terminator.

Core 2.0.2 keeps the bundled SDK system telemetry hooks disabled by default and
reports the maintained version through `getDevkitVersion()`. Defining
`ENABLETRACE=1` in platform build flags opts into that vendor behavior. This
does not disable or alter the application's explicit
[cloud uploader](../cloud/README.md). Review both behaviors when selecting a
different board package; no project-local symbol overrides remain.

## Verification and Related Guides

From the repository root:

```powershell
& .\firmware\tests\run-device-identity-tests.ps1 -Action Verify
& .\firmware\tests\run-telemetry-service-tests.ps1 -Action Verify
```

The first suite covers identity formatting and object state, plus the hardware
read when executed on the board. The second includes regression coverage for
the Core's float formatting. Neither command above executes tests on hardware.

There is no dedicated watchdog reset suite. Compile/link checks and a controlled
startup/recovery observation are relevant when changing watchdog or Core behavior.
Recheck resource usage after link-layout changes; known AZ3166 alignment warnings
must not be mistaken for runtime validation.

See [DeviceIdentity.cpp](DeviceIdentity.cpp),
[WatchdogController.cpp](WatchdogController.cpp), the
[telemetry guide](../telemetry/README.md), and the
[firmware design](../../../../docs/firmware-design.md#12-watchdog-and-blocking-behavior).