# Local Service Discovery

Give a local service a stable `.local` hostname and advertise its TCP endpoint
through IPv4 mDNS and DNS-SD. Discovery follows service availability and address
changes without depending on cloud credentials, Internet DNS, or NTP.

## Components and Design

| Component | Responsibility |
| --- | --- |
| [LocalDiscovery.h](LocalDiscovery.h) | Reconcile service availability, retry startup or transport failure, and expose the controller's running state. |
| `LocalDiscoveryService` | Caller-supplied hostname, instance/service name, TCP port, and encoded TXT metadata. |
| `LocalDiscoveryOperations` | Abstract clock, responder startup/shutdown, and health operations for replacement backends. |
| [MdnsTransport.h](MdnsTransport.h) | Abstract datagram contract consumed by the responder and implemented by protocol-test fakes. |
| [MdnsUdpTransport.h](MdnsUdpTransport.h) | Bounded, nonblocking lwIP multicast transport for AZ3166. |
| [mdns/README.md](mdns/README.md) | Vendored ArduinoMDNS responder; the application does not implement DNS packet handling itself. |

`LocalDiscovery::update(serviceAvailable, address)` starts the responder once a
service is available on a nonzero IPv4 address. Unchanged state does not cause
repeated registration. Address changes restart the binding; unavailability
releases the transport and records. A failed start or unhealthy transport waits
for the configured retry interval, measured from completion, before retrying.
A new address resets the previous address's retry delay.

The default backend owns one mDNS worker. It services queries independently of
the HTTP worker and the main loop, using a mutex around responder operations.
This keeps a slow HTTP client or synchronous cloud call from occupying the
mDNS polling loop.

## Reuse With an HTTP Service

Given the `handler` from the [HTTP guide](../http/README.md), this wiring exposes
the same service at `http://status-device.local:8080/status`:

```cpp
#include "src/discovery/LocalDiscovery.h"
#include "src/http/LocalWebServer.h"

const LocalDiscoveryService service = {
    "status-device",
    "status-device._http",
    8080,
    "\x0c" "path=/status"
};

LocalDiscovery discovery(5000, service);
LocalWebServer http(
    handler,
    8080,
    5000,
    mbed::callback(&discovery, &LocalDiscovery::update));
```

Use the hostname without `.local`; use the instance and service label without
the `._tcp.local` suffix. Match the advertised port to the actual listener. TXT
data is DNS-SD length-prefixed data, not a plain string: `path=/status` contains
12 bytes, so the example prefixes it with `\x0c`. Pass `NULL` for no TXT content.
Use short valid labels and metadata within the transport's packet capacity.

The HTTP server invokes discovery after successful listener startup, rather
than merely after Wi-Fi association. For another TCP service, supply its real
readiness and the current address to `update()` from one lifecycle owner. Use
the same high-octet-first IPv4 representation as
[ConnectivityManager](../connectivity/README.md); zero means unavailable.

The controller's availability input is state, not a connectivity-history queue.
Deliver an unavailable update between connection generations when the new
connection reuses the old address. The HTTP worker already performs this
reconciliation for its listener.

## Transport and Ownership

- The service descriptor is copied, but its string pointers are borrowed. Keep
  the hostname, service name, and TXT storage valid while used.
- Keep the discovery object alive until its caller, such as the HTTP server,
  has stopped issuing callbacks. Request unavailability before retiring the
  service; controller destruction alone is not the shutdown API.
- Call controller methods from one execution context. The default backend's
  responder mutex does not make the controller's own state thread-safe.
- The default platform backend has one shared responder per device. Constructing
  several controllers does not give each an independent advertisement service.
- To substitute lifecycle behavior, derive from `LocalDiscoveryOperations` and
  inject it by reference. The backend is borrowed and must outlive the controller.
  Independent fake implementations can own independent state.
- `MdnsUdpTransport` accumulates writes into one datagram until `endPacket()`.
  `parsePacket()` obtains the next packet, `read()` consumes buffered bytes, and
  `flush()` discards the current receive state. Configure the local address
  before joining a multicast group, and call `stop()` before rebinding.
- A replacement `MdnsTransport` must preserve datagram boundaries and report
  failed operations accurately. It is not a byte-stream socket interface.

## Limits and Dependencies

- Default transport: AZ3166 Core 2.0.0, Mbed RTOS, and lwIP multicast sockets on
  `224.0.0.251:5353`; this implementation is IPv4-only.
- Worker: fixed 4096-byte stack and one polling iteration every 20 milliseconds.
  Transport buffers: 1536 receive bytes and 512 send bytes; oversized packets
  are rejected rather than passed partially to the responder.
- Address and service TTLs are 120 seconds. Registration sends an announcement,
  followed by another after one second; periodic refresh is every 90 seconds.
  Withdrawal is best effort, so clients can retain cached names until expiry.
- The default service adapter advertises TCP. It does not configure router DNS,
  resolve names for clients, or guarantee delivery across VLANs or isolated Wi-Fi.
- No automatic conflict renaming is provided. Assign distinct hostnames when
  deploying several devices on one LAN. Advertisement adds no authentication
  or encryption to the advertised service.
- ArduinoMDNS retains its LGPL terms. Preserve the vendor license and local
  port notices when reusing or distributing it; see
  [THIRD_PARTY_NOTICES.md](../../../../THIRD_PARTY_NOTICES.md).

## Verification and Related Guides

From the repository root:

```powershell
& .\firmware\tests\run-local-discovery-tests.ps1 -Action Verify
```

The focused suite covers metadata, backend isolation, lifecycle and retries,
packet responses, malformed input, transport bounds, and cleanup. Compile-only
checks cannot establish that a particular LAN or browser permits mDNS. A live
check should resolve the chosen name and inspect the advertised service on an
mDNS-capable client, while retaining direct-IP access as a diagnostic fallback.

See [LocalDiscovery.cpp](LocalDiscovery.cpp),
[MdnsUdpTransport.cpp](MdnsUdpTransport.cpp), and the
[firmware design](../../../../docs/firmware-design.md#64-local-mdns-discovery).