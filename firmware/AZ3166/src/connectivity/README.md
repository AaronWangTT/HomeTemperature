# Connectivity Management

Maintain an AZ3166 station connection, retry failures, track the current IPv4
address, and report whether the system clock is synchronized. Applications can
use this component without the local web server, sensor service, or cloud uploader.

## Components and Design

| Component | Responsibility |
| --- | --- |
| [ConnectivityManager.h](ConnectivityManager.h) | Connection and clock state, retry policy, cached address, and update events. |
| `ConnectivityEvents` | One-update notifications for connection, disconnection, time synchronization, and address changes. |
| `ConnectivityOperations` | Replaceable clock, Wi-Fi, NTP, and address operations for deterministic testing or a platform adapter. |

The manager owns policy; its default operations call the installed AZ3166 APIs.
It does not own HTTP sockets, advertise services, or initiate cloud uploads.
The application decides how to use its state and events.

Each `update()` first maintains Wi-Fi, then checks time synchronization when
connected. Connection attempts reset station state before reconnecting. Failure
delays increase to the configured ceiling, and elapsed-time checks tolerate
`millis()` wraparound. A detected disconnect clears cached time readiness and
the address before a later update attempts reconnection.

## Reuse in Another Sketch

The constructor takes the status sampling interval, initial and maximum Wi-Fi
retry delays, and NTP retry interval, all in milliseconds:

```cpp
#include "src/connectivity/ConnectivityManager.h"

ConnectivityManager connectivity(1000, 5000, 60000, 60000);

void maintainConnectivity() {
    ConnectivityEvents events = connectivity.update();
    if (events.wifiConnected || events.localAddressChanged) {
        connectivity.printLocalHttpEndpoint("/status");
    }
}
```

Call the maintenance function from the main loop after the rest of the
application is initialized. This example only reports a URL: it does not create
a `/status` handler or an HTTP listener.

Use the state getters for ongoing decisions, not just the event flags:

- `isWiFiConnected()` reports the most recently sampled connection state.
- `localIPv4Address()` returns the cached address, with the first octet in the
  most significant byte: `192.0.2.1` is `0xC0000201`. Zero means unavailable.
- `isTimeSynchronized()` reports whether the platform clock is ready for uses
  such as TLS certificate validation.

For a local service, pass Wi-Fi and address state to its lifecycle API. For a TLS
client, additionally require synchronized time. A connected station can briefly
have no assigned address; wait for a nonzero address before advertising a service.

## Events and Address Handling

| Event | Meaning |
| --- | --- |
| `wifiConnected` | A connection attempt succeeded during this update. |
| `wifiDisconnected` | The sampled station connection was lost. |
| `timeSynchronized` | Connection found a synchronized clock, or a later NTP retry succeeded. |
| `localAddressChanged` | The cached IPv4 address changed, including acquisition or loss. |

Several flags can be true together. Events are returned by value and reset on
the next update; they are not an event queue. An address is sampled after
connection and at the configured status interval, so changes can be detected
without requiring a Wi-Fi disconnection. Reconnecting with the same address
still starts a new connectivity lifecycle.

The platform adapter guards missing address text before using the SDK parser.
`parseLocalIPv4Address()` and `nextRetryDelay()` expose the existing conversion
and backoff rules for focused tests; they do not access the network themselves.

## Ownership and Limits

- Give one execution context ownership of `update()` and the default Wi-Fi/NTP
  operations. The default backend uses the board's shared network interface;
  separate manager instances do not create independent radios.
- This is polling-based coordination, not an asynchronous network service.
  Wi-Fi connection and NTP calls are synchronous and can delay the calling loop.
  Retry intervals limit attempts but do not bound each platform call's duration.
- Getters return cached state. They do not sample the radio or provide a
  synchronized cross-thread snapshot.
- Wi-Fi provisioning remains owned by the board package, not this component.
  No cloud credentials are needed for local connectivity management.
- Derive from `ConnectivityOperations` and implement all seven methods to replace
  the platform backend. The injection constructor stores a non-owning reference;
  keep the backend and any state it borrows alive until the manager is destroyed.
  The default constructor uses a process-lifetime AZ3166 implementation.
- The interface permits instance-owned fake state but does not make Wi-Fi APIs
  thread-safe or change their synchronous execution.
- Serial helpers are diagnostics, not evidence that a particular HTTP listener
  is ready. `printLocalHttpEndpoint()` is suitable for the application's default
  port-80 URL; report custom ports from the owning service.

## Verification and Related Guides

From the repository root:

```powershell
& .\firmware\tests\run-connectivity-manager-tests.ps1 -Action Verify
```

Tests exercise retry timing, connection transitions, NTP readiness, address
parsing and changes, and timer wraparound using controlled platform operations.
Compile checks do not validate actual access-point or DHCP behavior.

See [ConnectivityManager.cpp](ConnectivityManager.cpp), the
[HTTP guide](../http/README.md), the [cloud guide](../cloud/README.md), and the
[firmware design](../../../../docs/firmware-design.md#6-connectivity-design).