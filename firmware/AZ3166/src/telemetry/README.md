# Sensor Telemetry and HTTP Adapter

Acquire the AZ3166's temperature, humidity, and pressure readings and format a
single validated JSON payload for local HTTP or cloud delivery. Acquisition and
formatting are reusable without either network path; the HTTP adapter is kept
here so the generic HTTP engine stays independent of environmental telemetry.

## Components and Design

| Component | Responsibility |
| --- | --- |
| [TelemetryService.h](TelemetryService.h) | Own sensor objects, serialize acquisition, validate readings, and build payloads in caller-provided storage. |
| `TelemetryReading` | Temperature, humidity, and pressure values passed between acquisition and formatting. |
| [TelemetryHttpHandler.h](TelemetryHttpHandler.h) | Implement the application's exact telemetry route and map payload outcomes to HTTP responses. |
| `TelemetryHttpPayloadBuilder` | Optional callback/context for substituting payload production in the HTTP adapter. |

```text
TelemetryService::buildPayload
    -> read sensors under a short mutex
    -> validate and format into caller storage

TelemetryHttpHandler -> LocalHttpResponse
TelemetryUploader   -> cloud transport
```

The same `TelemetryService` can serve the HTTP worker and main-loop uploader.
Only the three sensor calls hold the instance-owned mutex. Formatting and
network operations do not hold that lock, and each caller owns its temporary
reading and payload buffer. A payload request acquires a new reading; the
component does not maintain a background sampling cache.

## Reuse Acquisition or Formatting

Initialize the service once with an application-owned identifier before either
HTTP or cloud code accesses it:

```cpp
#include <Arduino.h>
#include "src/telemetry/TelemetryService.h"

TelemetryService telemetry;
const char deviceId[] = "example-device";

void setup() {
    Serial.begin(115200);
    telemetry.begin(deviceId);
}

void loop() {
    char payload[160];
    int length = telemetry.buildPayload(payload, sizeof(payload));
    if (length > 0) {
        Serial.write(reinterpret_cast<const uint8_t *>(payload), length);
        Serial.println();
    }
    delay(1000);
}
```

Use `read(TelemetryReading&)` when you need values rather than JSON. A successful
read means all three driver calls succeeded; range and finite-value validation
is performed by the formatter. Use the static formatter for already-acquired
readings, without constructing sensor objects:

```cpp
TelemetryReading reading = {23.5f, 45.0f, 1013.2f};
char payload[160];
int length = TelemetryService::formatPayload(
    "example-device", reading, payload, sizeof(payload));
```

The output uses this schema, with one fractional digit per measurement:

```json
{
  "deviceId": "example-device",
  "temperature": 23.5,
  "humidity": 45.0,
  "pressure": 1013.2
}
```

| Measurement | Unit | Accepted range |
| --- | --- | --- |
| Temperature | Degrees Celsius | -50 to 100 |
| Humidity | Percent relative humidity | 0 to 100 |
| Pressure | hPa | 300 to 1200 |

These ranges and field names are application choices matching ingestion, not a
general-purpose sensor serialization standard. Adapt
[AppConfig.h](../config/AppConfig.h) and the formatter if another application
has a different contract.

## Reuse the HTTP Adapter

Construct `TelemetryHttpHandler handler(telemetry)` and inject it into
`LocalWebServer`; see the [HTTP guide](../http/README.md). The adapter recognizes
the request-line prefix `GET /api/telemetry `, including the trailing space.
Other methods, subpaths, and query strings do not match this route.

| Condition | Result |
| --- | --- |
| Valid reading and payload | `200 OK`, JSON body |
| Unknown route | `404 Not Found`, without sensor acquisition |
| Sensor failure or invalid measurement | `503 Service Unavailable` |
| Missing builder, formatting failure, or insufficient output storage | `500 Internal Server Error` |

The alternate constructor takes a payload-builder callback and borrowed context,
which lets a test provide exact outcomes without real sensors. To support a
different protocol or route, implement `LocalHttpHandler` instead of adding
application-specific branches to the HTTP engine.

## Ownership and Limits

- The service borrows the identifier string. It must remain valid and immutable
  after `begin()`. The formatter does not JSON-escape arbitrary identifiers;
  use a known-safe identifier such as the one from the
  [platform component](../platform/README.md#deviceidentity).
- Construct and initialize one service for the board's shared sensor bus.
  Separate service instances have separate mutexes and therefore do not protect
  one another's access to the same physical I2C bus.
- Acquisition uses HTS221, LPS22HB, and `DevI2C` on pins D14/D15. Another board or
  sensor set requires adapting acquisition. The bundled driver headers and
  platform runtime are still build dependencies even for the static formatter.
- `buildPayload()` and `formatPayload()` return a positive byte count on success,
  `PAYLOAD_SENSOR_ERROR` (-1) for invalid/unavailable measurements, or
  `PAYLOAD_FORMAT_ERROR` (-2) for invalid arguments or formatting failures.
  Do not transmit the buffer after a failure; it may contain incomplete data.
- AZ3166 Core 2.0.2 supplies the corrected `dtostrf` behavior covered by the
  formatter regression tests.
- There is one acquisition attempt per request, with no retries or calibration
  workflow. Cloud retry scheduling belongs to the [cloud components](../cloud/README.md).

## Verification

From the repository root:

```powershell
& .\firmware\tests\run-telemetry-service-tests.ps1 -Action Verify
& .\firmware\tests\run-local-web-server-tests.ps1 -Action Verify
```

The telemetry suite covers formatting, range checks, buffer limits, identity
handling, and onboard acquisition when run on hardware. The HTTP suite checks
the adapter's route and status mapping using fake payload builders. The commands
above compile those suites; they do not execute hardware assertions.

See [TelemetryService.cpp](TelemetryService.cpp),
[TelemetryHttpHandler.cpp](TelemetryHttpHandler.cpp), and the
[firmware design](../../../../docs/firmware-design.md#8-telemetry-pipeline).