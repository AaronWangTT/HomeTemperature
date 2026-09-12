# Platform Support and Compatibility

Keep board identity, watchdog recovery, and AZ3166 Core compatibility behavior
in one place. Identity and watchdog are small reusable wrappers; the compatibility
files deliberately affect process-wide SDK behavior and are not general utilities.

## Components

| Component | Responsibility |
| --- | --- |
| [DeviceIdentity.h](DeviceIdentity.h) | Read the STM32 unique identifier and provide a stable application ID. |
| [WatchdogController.h](WatchdogController.h) | Configure the board watchdog, report its reset cause, and feed its timer. |
| [FloatFormatting.cpp](FloatFormatting.cpp) | Supply the project's corrected C-linkage `dtostrf` implementation for AZ3166 Core 2.0.0. |
| [disable_system_telemetry.cpp](disable_system_telemetry.cpp) | Replace SDK system-telemetry hooks with no-op definitions without modifying the installed board package. |

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

## Float Formatting Override

Core 2.0.0's `dtostrf` has incorrect fractional-digit behavior. The local override
keeps the same C-linkage function signature and supplies rounding, width/padding,
and handling of non-finite or out-of-range values. The sensor formatter uses it
instead of relying on problematic floating-point `printf` behavior in this core.

There is no component object to construct. Include the translation unit in the
firmware build, as the recursive `src/` build already does. Do not `#include` the
implementation file into another source file or add a second competing override.

The function writes into caller-owned storage and has no capacity parameter;
callers must size the output for their requested width, precision, sign, and
terminator. Review this workaround when upgrading the board package rather
than assuming every future core needs the replacement.

## SDK Telemetry Override

The no-op C definitions replace `telemetry_init`, `send_telemetry_data`, and the
corresponding asynchronous and synchronous send hooks linked from the SDK.
This prevents those SDK hooks from sending unrelated system telemetry while
leaving the application's [cloud uploader](../cloud/README.md) intact.

This is a firmware-wide link decision, not a runtime privacy toggle. The exact
symbol names and signatures must match the installed SDK. Reusing the file on
another SDK requires checking those definitions and link behavior; it is not
a general guarantee that every possible network egress path is disabled.

## Verification and Related Guides

From the repository root:

```powershell
& .\firmware\tests\run-device-identity-tests.ps1 -Action Verify
& .\firmware\tests\run-telemetry-service-tests.ps1 -Action Verify
```

The first suite covers identity formatting and object state, plus the hardware
read when executed on the board. The second includes float-formatting regression
coverage. Neither command above executes the tests on hardware.

There is no dedicated watchdog reset suite. Compile/link checks and a controlled
startup/recovery observation are relevant when changing watchdog or SDK overrides.
Recheck resource usage after link-layout changes; known AZ3166 alignment warnings
must not be mistaken for runtime validation.

See [DeviceIdentity.cpp](DeviceIdentity.cpp),
[WatchdogController.cpp](WatchdogController.cpp), the
[telemetry guide](../telemetry/README.md), and the
[firmware design](../../../../docs/firmware-design.md#12-watchdog-and-blocking-behavior).