# Local HTTP Service

Run a small LAN HTTP service on the AZ3166 without tying request handling to
the Arduino main loop. The worker can serve clients while the main loop waits
for a cloud upload. It has no dependency on sensors, telemetry JSON, cloud
credentials, or mDNS.

## Components and Design

| Component | Responsibility |
| --- | --- |
| [LocalWebServer.h](LocalWebServer.h) | Worker lifecycle, listener readiness, connectivity reconciliation, and bounded HTTP I/O. |
| [LocalHttpHandler.h](LocalHttpHandler.h) | Application request handler and response contract: status, content type, and exact body length. |
| [LocalHttpStreamingHandler.h](LocalHttpStreamingHandler.h) | Optional request-body routing, bounded pull-stream API, and streaming limits. |
| `LocalWebServerOperations` | Replaceable clock and socket operations for focused tests or another transport adapter. |
| `LocalHttpServiceUpdate` | Optional typed callback for listener availability, independent of the application handler. |

```mermaid
flowchart LR
    Loop[Main loop] -->|Wi-Fi and IPv4 snapshot| Worker[HTTP worker]
    Worker -->|request line and response buffer| Handler[Application handler]
    Worker -->|validated request and socket ownership| BodyWorker[Streaming worker]
    BodyWorker -->|bounded body chunks| StreamHandler[Streaming handler]
    Handler -->|status, type, byte count| Worker
    Worker -->|listener availability| Callback[Optional service callback]
```

`update(wifiConnected, address)` publishes desired connectivity; it does not
handle a request on the calling thread. One worker owns the listener and
ordinary client sockets; the optional streaming worker owns each socket
transferred to it. The listener worker checks actual socket open/bind/listen
results, retries failures, and uses a connection generation to discard outdated
startup and I/O work.
`state()` returns a synchronized readiness/address/error snapshot.

## Reuse in Another Sketch

Implement a handler and supply the port and startup retry interval. This example
serves a plain-text status route without the telemetry implementation:

```cpp
#include <string.h>
#include "src/http/LocalWebServer.h"

class StatusHandler : public LocalHttpHandler {
public:
    LocalHttpResponse handle(const char *requestLine, char *body,
                             size_t capacity) override {
        const char route[] = "GET /status ";
        bool found = strncmp(requestLine, route, sizeof(route) - 1) == 0;
        const char *message = found ? "ready\n" : "not found\n";
        size_t length = strlen(message);
        if (length > capacity) {
            return {"500 Internal Server Error", "text/plain", 0};
        }
        memcpy(body, message, length);
        return {found ? "200 OK" : "404 Not Found", "text/plain", length};
    }
};

StatusHandler handler;
LocalWebServer http(handler, 8080, 5000);
```

After application initialization, call `http.update(wifiConnected, ipv4Address)`
from the main loop whenever reconciling connectivity. The address uses the first
IPv4 octet in the most significant byte: `192.0.2.1` is `0xC0000201`. Pass zero
when no address is available. `update(false, 0)` requests listener shutdown but
retains the worker for a later reconnect.

The existing [TelemetryHttpHandler.h](../telemetry/TelemetryHttpHandler.h) is one
application adapter, not part of the HTTP engine. Another handler can expose a
different route, payload, or content type using the same server.

## Optional Request-Body Streaming

Existing `LocalHttpHandler` implementations remain request-line handlers. To
accept a bounded body without buffering it in RAM, also implement
`LocalHttpStreamingHandler` and use a streaming constructor with explicit
`LocalHttpStreamingLimits`. `handles()` selects body routes on the listener
worker. A selected request requires exactly one canonical decimal
`Content-Length`; duplicate or malformed lengths, any `Transfer-Encoding`,
lengths over `maxContentLength`, and prefetched bytes beyond the declared length
are rejected before ownership transfer.

The listener transfers the accepted socket and all bytes already read after
`\r\n\r\n` to one joinable streaming worker. `LocalHttpBodyStream::read()`
delivers those prefetched bytes first and then reads bounded chunks from the
socket under both idle and total deadlines. It reports completion, disconnect,
timeout, cancellation, and backend errors explicitly. The streaming handler
must consume exactly the declared length before returning a successful response.
Only one streaming request can be active; another receives `503` while ordinary
bounded handlers continue on the listener worker.

Each request carries the current connectivity generation.
`cancelStreamingRequest(generation)` affects only the matching active request,
and disconnect, address change, or server shutdown also makes its body stream
observe cancellation. Cancellation is cooperative: streaming handlers must keep
their own work bounded and check `LocalHttpBodyStream::cancelled()` while waiting
outside `read()`.

## Advertisement and Ownership

Pass `mbed::callback(&discovery, &LocalDiscovery::update)` as the optional final
constructor argument to connect an existing discovery object. See the
[discovery guide](../discovery/README.md) for its metadata configuration.

- Service callbacks run on the HTTP worker after successful listener startup and
  while it remains available. They are repeated reconciliation calls, not only
  one-shot transition notifications.
- The handler and callback target must outlive the server. A copied Mbed callback
  does not own its target. Status and content-type strings returned by a handler
  must remain valid until transmission finishes.
- Shared application state needs its own synchronization. Keep sensor locks
  short and release them before network operations.
- Destroying the server requests both workers to stop, wakes and cancels the
  streaming worker, and joins both. Handlers and callbacks must finish within
  their documented bounds; neither may destroy the server from its own worker.
- Derive from `LocalWebServerOperations` to replace the clock and socket backend.
  The injection constructor borrows it by reference: it must outlive the server,
  including worker shutdown and join. The default backend has process lifetime.
- A backend shared by listener and streaming workers must return `true` from
  `supportsConcurrentSockets()` and support concurrent operations on independent
  descriptors without a global blocking lock. Otherwise pass independent
  listener and streaming backend instances that share the descriptor namespace.
  Both instances must outlive the server and all socket owners.
- Custom operations must use bounded/nonblocking I/O. Their clock can be called
  from the main loop and both HTTP workers; keep referenced state valid and
  synchronized.

## Limits and Dependencies

- AZ3166 Core 3.0.0, Mbed RTOS, and lwIP are required by the default backend.
- Ordinary clients are handled one at a time. Configuring streaming adds one
  6144-byte worker stack and permits one body request concurrently with bounded
  listener requests; it does not permit concurrent body uploads.
- Request line: 96 bytes; total request headers: 2048 bytes; response body:
  512 bytes; prefetched body: at most 128 bytes. Header reads and response writes
  each have a two-second deadline. Body size, idle deadline, and total deadline
  are explicit constructor limits.
- Responses include `Content-Length` and `Connection: close`; partial writes are
  handled. Bodies may contain binary data within the supplied buffer limit.
- Persistent connections, chunked transfer coding, WebSockets, TLS, and
  authentication are not implemented. Use this service only on a trusted LAN.
- A slow client can delay another client. Threads do not remove network limits
  or bound arbitrary handler execution. The main-loop watchdog does not provide
  a separate HTTP-worker health check.

## Verification and Related Guides

From the repository root, compile the focused suite:

```powershell
& .\firmware\tests\run-local-web-server-tests.ps1 -Action Verify
```

The suite covers routing adapters, worker execution, listener lifecycle,
callback binding, partial I/O, binary responses, bounds, timeouts, and cleanup.
Board runs use `-Action Run -Port COMx` with the detected ST-Link port and restore
production afterward. Do not treat a compile-only result as a runtime pass.

See [LocalWebServer.cpp](LocalWebServer.cpp), the
[telemetry guide](../telemetry/README.md), and the
[firmware design](../../../../docs/firmware-design.md#9-local-http-interface).
The [Core HTTP server proposal](../../../../docs/core-http-server-design.md)
describes how a future Core could absorb the reusable HTTP transport and
lifecycle behavior without weakening this service contract.