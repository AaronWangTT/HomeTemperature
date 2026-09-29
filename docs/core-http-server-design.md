# AZ3166 Core HTTP Server Design

Status: proposed

Baseline: AZ3166 Core 3.1.3

Target: a future maintained AZ3166 Core release after 3.1.3

Initial consumer: HomeTemperature local telemetry

## 1. Decision

Improve the Core HTTP server with the bounded transport and lifecycle behavior
already proven by HomeTemperature, then replace the application-owned socket
engine with a thin Core adapter. Do not run both listeners or copy the Core
HTTPD implementation into HomeTemperature.

The current HomeTemperature implementation remains authoritative until a
released, checksum-pinned Core provides equivalent behavior and passes the
consumer validation below.

## 2. Motivation

Core 3.1.3 provides HTTP parsing, method dispatch, a WSGI-style route
table, standard error responses, response-header helpers, and partial-send
handling. Reusing those facilities would avoid maintaining a second HTTP
protocol implementation in the application.

It is not currently an equivalent replacement for HomeTemperature's
`LocalWebServer`. Its listener uses global state, binds fixed port 80 to
`INADDR_ANY`, reports thread creation before bind and listen complete, performs
blocking request and response I/O without an overall deadline, and can spend up
to 20 seconds stopping. Its route callbacks have no application context, and
its transport cannot be replaced in focused tests.

## 3. Design Boundary

The Core should own:

- listener and accepted sockets;
- request-line and header parsing;
- HTTP method and route dispatch;
- response framing and partial writes;
- bounded request and response deadlines;
- listener readiness, errors, retry, and shutdown; and
- transport-level tests.

HomeTemperature should continue to own:

- Wi-Fi policy and the current IPv4 snapshot;
- telemetry routing and payload generation;
- HTTP status, content type, and body selection;
- mDNS service metadata and advertisement policy; and
- application-level and Core-integration tests.

The Core must remain independent of HomeTemperature, telemetry, ArduinoMDNS,
cloud credentials, and sensor drivers.

## 4. Core Changes

Preserve `httpd_init()`, `httpd_start()`, and the existing WSGI API for current
sketches. Add compatible APIs rather than changing existing aggregate
initializers or callback signatures in place.

The enhanced server should provide:

- configurable port, bind address, backlog, stack size, and I/O deadlines;
- a start result that is returned only after socket, bind, and listen complete;
- observable starting, listening, stopped, and failed states;
- the bound address, port, and last transport error;
- nonblocking listener and client sockets;
- bounded reads, header consumption, and response writes;
- safe cancellation when connectivity generation changes;
- bounded stop and worker join behavior;
- a route form containing a caller-owned context pointer; and
- replaceable clock and socket operations for deterministic tests.

The enhanced lifecycle API must let a caller rebind or disconnect a running
instance without reaching into Core internals:

```cpp
int httpd_reconfigure(const httpd_config_v2_t *config);
int httpd_stop(unsigned timeout_ms);
```

`httpd_reconfigure()` accepts a new bind address and port from the caller's
current connectivity snapshot. If either value differs from the active listener,
or if the caller passes a null address to represent Wi-Fi disconnect, the Core
must immediately mark listener readiness false, cancel the active listener and
clients, join the worker within the configured stop bound, and restart
synchronously with the new endpoint. The call returns only after the server is
listening, stopped for disconnect, failed, or the bounded restart times out.
`httpd_stop()` performs the same readiness invalidation and bounded cancellation
without starting a replacement listener. Stale readiness from a previous Wi-Fi
generation or bind address must never remain observable after either call
begins.

An extended route can use one callback plus a method mask:

```cpp
typedef int (*httpd_handler_v2_t)(
    httpd_request_t *request,
    void *context);

typedef struct {
    const char *uri;
    unsigned methods;
    int header_fields;
    int flags;
    httpd_handler_v2_t handler;
    void *context;
} httpd_route_v2_t;
```

Exact matching must have a documented policy for query strings and trailing
slashes. HomeTemperature requires `GET /api/telemetry` to reject other methods,
subpaths, query strings, and trailing slashes.

## 5. HomeTemperature Adapter

After the enhanced Core is released, `LocalWebServer` can become a thin adapter
that:

1. registers `/api/telemetry`;
2. forwards a structured request to `LocalHttpHandler`;
3. maps `LocalHttpResponse` to the Core response API;
4. publishes actual listener readiness to `LocalDiscovery`;
5. reconciles Wi-Fi disconnects and address changes; and
6. exposes the Core listener state for diagnostics.

`LocalHttpHandler`, `LocalHttpResponse`, and `TelemetryHttpHandler` should remain
application-owned so telemetry behavior stays independent of the transport.
The application retains a bounded 3072-byte response limit, exact
`Content-Length`, `Connection: close`, and current 200, 400, 404, 500, and 503
response behavior.

## 6. Compatibility and Resources

Core 3.1.3 uses a singleton HTTPD, fixed port 80, and an 8192-byte worker stack.
HomeTemperature currently uses one configurable listener plus separate
listener and streaming workers with 8192-byte stacks. Before adoption, compare
linked flash, `.data`, `.bss`, heap low-water mark, and worker stack usage.

The first enhanced Core release may remain single-instance, but that limitation
must be explicit. Duplicate route registration, unregister, stop, restart, and
reinitialization must have deterministic results. TLS is outside this proposal;
the local service remains restricted to a trusted LAN.

## 7. Migration Plan

1. Complete the Core 3.1.3 migration without changing the current HTTP engine.
2. Record production and test firmware size and runtime behavior as a baseline.
3. Add failing Core tests for listener readiness, timeouts, partial I/O,
   shutdown, route context, and restart.
4. Implement the compatible Core extensions and validate existing SystemWeb
   behavior.
5. Publish an immutable Core release and Board Manager index entry.
6. Upgrade HomeTemperature through its Core-version workflow.
7. Replace only the transport and lifecycle portion of `LocalWebServer`.
8. Remove the raw-lwIP adapter after all consumer checks pass.

## 8. Acceptance Criteria

The proposal is complete when:

- start succeeds only after the listener is ready;
- bind, listen, accept, read, write, and worker errors are observable;
- incomplete requests and stalled responses cannot exceed configured deadlines;
- stop completes within a documented bound without blocking the Arduino main
  loop for seconds;
- Wi-Fi loss, reconnect, and IPv4 changes cannot publish stale readiness;
- mDNS advertisement follows actual HTTP availability;
- route callbacks receive application context without global trampolines;
- existing Core HTTP users remain source-compatible;
- HomeTemperature preserves its current HTTP response contract;
- focused Core tests retain transport fault injection;
- production firmware and all firmware test sketches compile; and
- hardware tests cover slow clients, disconnects, reconnects, address changes,
  repeated requests, mDNS discovery, and concurrent cloud upload.

## 9. Non-goals

- Add TLS, authentication, WebSockets, or persistent connections.
- Move telemetry, discovery, or Wi-Fi policy into the Core.
- Support multiple simultaneous clients in the first release.
- Remove the HomeTemperature implementation before the enhanced Core is
  released and validated.
