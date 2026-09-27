# AZ3166 Core Multicast UDP Design

Status: implemented in AZ3166 Core 3.0.0

Core component: `AZ3166MulticastUDP`

Initial consumer: HomeTemperature local mDNS discovery

## 1. Decision

Move reusable multicast UDP capability into the AZ3166 Core instead of moving
HomeTemperature's `MdnsUdpTransport` class into the Core unchanged.

The preferred end state was to extend the Core's existing `WiFiUDP` class with
correct packet-oriented send behavior, multicast membership, packet-oriented
receive behavior, and observable transport errors. This would make the
capability useful to any AZ3166 sketch and keep the Core independent of
ArduinoMDNS.

Compatibility and memory validation favored the fallback: a generic
`AZ3166MulticastUDP` class in the Core's WiFi library. It reuses the proven
behavior without an mDNS-specific name and does not depend on HomeTemperature
or ArduinoMDNS.

AZ3166 Core 3.0.0 provides that class. HomeTemperature pins the immutable Core
release and uses `AZ3166MulticastUDP` directly; the former local adapter has
been removed.

## 2. Context

ArduinoMDNS constructs one DNS message through this transport sequence:

1. `beginPacket(destination, port)`
2. one or more `write(buffer, size)` calls
3. `endPacket()`

It receives one DNS message through this sequence:

1. `parsePacket()`
2. one or more `read(buffer, size)` calls
3. `flush()` when the remaining packet must be discarded

The maintained ArduinoMDNS transport wrapper accepts any borrowed object with
that method shape. It does not require an mDNS-specific transport type.

The original AZ3166 Core `WiFiUDP` implementation was not compatible with those
semantics:

- it did not expose `beginMulticast()` or `parsePacket()`;
- each `write()` called `sendto()` immediately instead of appending to one packet;
- `endPacket()` reported success without sending or validating anything;
- `flush()` did not discard receive state;
- bind and send results were not consistently propagated; and
- it was not derived from the conventional Arduino `UDP` interface.

HomeTemperature therefore originally owned `MdnsUdpTransport`, a raw-lwIP
adapter that joined the mDNS group on the active local interface, preserved
datagram boundaries, used bounded buffers, operated nonblockingly, and exposed
a sticky failure state to the discovery lifecycle.

The low-level socket behavior belongs in the board package. The worker thread,
mDNS service description, retry policy, and responder lifecycle remain
application concerns.

## 3. Goals

- Provide reusable IPv4 multicast UDP support in the AZ3166 Core.
- Preserve complete UDP datagrams during receive processing.
- Support the maintained ArduinoMDNS structural transport contract.
- Keep socket operations nonblocking for polling-based applications.
- Reject overflow and truncation instead of emitting or consuming partial
  protocol messages silently.
- Expose enough error state for a long-running service to detect a failed socket
  and reinitialize it.
- Use bounded, deterministic memory with no allocation per packet.
- Preserve existing unicast UDP behavior.
- Keep lwIP types and implementation details out of the public header.

## 4. Non-goals

- Implement mDNS, DNS-SD, hostname conflict handling, or service records in the
  Core.
- Move HomeTemperature's discovery worker, retry policy, or service lifecycle
  into the Core.
- Add IPv6 multicast in the first version.
- Support multiple network interfaces or Soft AP mode in the first version.
- Guarantee multicast forwarding across VLANs or wireless client isolation.
- Change ArduinoMDNS parsing or response behavior.

## 5. Implemented Public API

The first implementation uses a separate class:

```cpp
class AZ3166MulticastUDP {
public:
    AZ3166MulticastUDP();
    ~AZ3166MulticastUDP();

    void setLocalIPv4Address(IPAddress address);
    bool failed() const;

    uint8_t beginMulticast(IPAddress group, uint16_t port);
    void stop();
    int beginPacket(IPAddress destination, uint16_t port);
    size_t write(const uint8_t *buffer, size_t size);
    int endPacket();
    int parsePacket();
    int read(uint8_t *buffer, size_t size);
    void flush();
    IPAddress remoteIP();
    uint16_t remotePort();
};
```

HomeTemperature supplies the active station address through
`setLocalIPv4Address()` before joining the multicast group. The transport
configures unicast and multicast TTL 255 for link-local discovery traffic.

`failed()` is sticky for receive, send, and outbound-overflow failures until
`stop()` or a new `beginMulticast()` attempt resets the transport.

## 6. Required Semantics

### 6.1 Lifecycle

- Construction produces a closed, non-failed object with initialized counters.
- `beginMulticast()` closes any previous session before opening a new one.
- `stop()` is idempotent and releases the socket and packet state.
- Copy construction and copy assignment are disabled.
- The class is not internally thread-safe. A caller serializes operations on an
  instance.

### 6.2 Multicast setup

A successful `beginMulticast()` performs all of the following:

- creates an IPv4 datagram socket;
- enables address reuse;
- binds the requested local port;
- joins the requested group on the selected station interface;
- selects that interface for multicast transmission;
- applies unicast and multicast TTL 255; and
- switches the socket to nonblocking mode.

Every socket operation is checked. A partial setup is closed before failure is
returned.

### 6.3 Sending

- `beginPacket()` validates an open socket and port, then clears the pending
  outbound packet.
- `write()` appends to the pending packet and returns the number of bytes
  accepted.
- A write that would exceed capacity rejects the whole write, marks the pending
  packet invalid, and records an overflow error.
- `endPacket()` performs exactly one `sendto()` call.
- Success is reported only when `sendto()` transmits the complete pending
  packet.
- No truncated or partially assembled datagram is sent.

### 6.4 Receiving

- `parsePacket()` first discards any unread previous packet.
- It performs a nonblocking receive and stages at most one complete datagram.
- It returns zero when no packet is available and the payload length on success.
- `read()` never crosses the current packet boundary.
- `flush()` discards only the staged packet and is otherwise idempotent.
- `remoteIP()` and `remotePort()` describe the staged packet's sender.

## 7. Memory Model

The implementation does not allocate or free memory for each packet. It uses:

- receive capacity: 1536 bytes;
- send capacity: 512 bytes.

These capacities are named Core constants and documented as per-instance RAM
cost. A separate class avoids increasing every ordinary `WiFiUDP` object's size.

## 8. Compatibility

Existing `WiFiUDP` behavior remains unchanged. Multicast-aware users opt into
`AZ3166MulticastUDP`, so sketches that rely on the existing immediate-write
behavior are not silently changed.

Full inheritance from Arduino's conventional `UDP` base class remains a possible
future compatibility step. It is not required for HomeTemperature because
maintained ArduinoMDNS uses structural type erasure.

## 9. Validation

### 9.1 Core checks

Core host tests cover:

- construction, destruction, and repeated `stop()`;
- failed socket creation, bind, membership, and socket configuration;
- multicast join and interface selection;
- multiple `write()` calls producing exactly one datagram;
- outbound overflow producing no datagram;
- complete-send and error-send handling;
- one-packet-at-a-time receive boundaries;
- `flush()`, sender address, and sender port; and
- sticky failure and reset behavior.

The production base and Azure-enabled sketch inventories compile in CI. The
protocol-neutral multicast example does not depend on ArduinoMDNS.

A physical AZ3166 test also exchanged multicast datagrams with a LAN peer:

- the board joined `239.255.0.1:5000`;
- a host sent a 22-byte multicast probe;
- the board reported the sender address and port;
- the board returned `AZ3166 multicast reply`; and
- OpenOCD reported `Verified OK` for the test image.

The complete pre-test 1 MiB Flash image was restored afterward. OpenOCD reported
`Verified OK`, and a full read-back SHA-256 matched the original backup.

### 9.2 Consumer checks

HomeTemperature:

- installs the immutable Board Manager 3.0.0 release and verifies its archive
  checksum;
- uses the Core transport instead of `MdnsUdpTransport`;
- preserves mDNS TTL 255 and nonblocking operation;
- retains fake-transport ArduinoMDNS parser tests;
- retains a focused Core-transport integration test;
- compiles the production firmware and all firmware test sketches; and
- records Core and ArduinoMDNS provenance independently.

Live `.local` and DNS-SD behavior remains environment-dependent and should be
checked on each deployment network.

## 10. Completed Migration

1. Added focused Core multicast transport tests.
2. Implemented `AZ3166MulticastUDP` with an isolated raw-lwIP backend.
3. Validated Core base and Azure-enabled profiles.
4. Published immutable AZ3166 Core 3.0.0 and its base-profile archive.
5. Published the Board Manager entry from a reviewed immutable index commit.
6. Upgraded HomeTemperature's Core and ArduinoMDNS pins.
7. Replaced the local transport member with `AZ3166MulticastUDP`.
8. Removed `MdnsUdpTransport.cpp`, `MdnsUdpTransport.h`, and their test-staging
   entries.
9. Updated discovery and third-party documentation to describe Core ownership.

## 11. Acceptance Criteria

- ArduinoMDNS emits one valid UDP datagram when it uses multiple writes.
- The AZ3166 joins `224.0.0.251:5353` on the active station interface.
- Incoming packet boundaries and sender metadata remain intact.
- Socket and send failures are observable by HomeTemperature.
- Malformed or oversized traffic cannot cause out-of-bounds access or silent
  outbound truncation.
- Existing supported unicast UDP behavior remains unchanged.
- RAM and flash deltas were measured and accepted.
- HomeTemperature no longer carries a raw-lwIP UDP adapter.

## 12. Follow-up Options

- Evaluate conventional Arduino `UDP` inheritance if another consumer requires
  that interface.
- Add IPv6 multicast only with a separate design and compatibility review.
- Add automated trusted-board execution when CI has suitable hardware and
  network fixtures.
