# AZ3166 Local OTA Design

Status: proposed

Baseline: AZ3166 Core 3.0.0

Target: a future maintained AZ3166 Core release after 3.0.0 and a later
HomeTemperature integration

## 1. Decision

Support local firmware upload without requiring the device to download an image
from a remote URL.

The Core should provide a transport-independent, streaming OTA staging API that
writes a verified application image to the existing external Flash OTA
partition and activates it through the existing bootloader contract.
HomeTemperature should provide the local HTTP endpoint, physical authorization,
product policy, progress reporting, and reboot coordination.

The initial implementation must not replace the bootloader or claim rollback
support. A failed update must remain recoverable through ST-Link.

## 2. Core 3.0.0 Findings

The Core's
[`OTAFirmwareUpdate`](https://github.com/AaronWangTT/devkit-sdk/blob/6cd046137cb3e054a343e9a4a5a5323cf566f3e6/libraries/OTA/src/OTAFirmwareUpdate.cpp)
library exposes two operations:

- `OTADownloadFirmware()` performs an HTTP or HTTPS GET, writes response chunks
  directly to `MICO_PARTITION_OTA_TEMP`, and computes the MICO CRC16.
- `OTAApplyNewFirmware()` passes the downloaded length and CRC16 to
  `mico_ota_switch_to_new_fw()`. The bootloader applies the staged image after a
  reboot.

The application image is a raw `.bin` linked at `0x0800C000`. Inspection of the
Core 3.0.0 partition table shows:

| Partition | Storage | Start | Capacity |
| --- | --- | ---: | ---: |
| Application | Internal Flash | `0x0800C000` | `0xF4000` / 999,424 bytes |
| OTA temporary | External QSPI Flash | `0x00070000` | `0xF4000` / 999,424 bytes |

The runtime value returned by `MicoFlashGetInfo(MICO_PARTITION_OTA_TEMP)` must
remain authoritative; applications must not duplicate these constants.

The existing implementation is useful as a proof of the bootloader path, but
it is too narrow for a safe local uploader:

- download transport, staging, checksum, and activation are one workflow;
- state is held in global variables and is not reentrant;
- the source does not explicitly erase the OTA partition before writing;
- the incoming size is not checked against the partition capacity;
- a failed or short write is not reported to the HTTP client while streaming;
- there is no read-back verification;
- CRC16 detects accidental corruption but does not authenticate firmware;
- there is no board identity, package format, version, or downgrade policy;
- there is no progress, cancellation, or explicit state model; and
- no rollback or post-boot health-confirmation interface is exposed.

The precompiled implementation of `mico_ota_switch_to_new_fw()` writes the OTA
partition address, image length, application/upgrade markers, and CRC16 into the
MICO boot table. Its return path does not reliably propagate the result of
persisting that boot table. This behavior must be corrected or wrapped with
independent verification before an application reports that activation is
ready.

## 3. ESP32-S3 Reference

The local
[`remote-keyboard-connector`](https://github.com/AaronWangTT/remote-keyboard-connector/tree/60a7a591f4c1747a3b2f79cdfcba3b4c21944391)
implementation provides a useful architectural reference:

- its
  [`firmware_update`](https://github.com/AaronWangTT/remote-keyboard-connector/blob/60a7a591f4c1747a3b2f79cdfcba3b4c21944391/components/firmware_update/firmware_update.c)
  component owns an explicit receive, verify, stage, activate, and failure
  state machine;
- the HTTP layer validates admission and streams bounded chunks through a
  dedicated upload worker;
- upload completion and activation are separate operations tied to a job ID and
  verified digest;
- compatibility metadata identifies the product, board, Flash layout, source,
  version, and security profile;
- the host tooling independently validates and signs release artifacts; and
- the new application must pass health checks before ESP-IDF cancels rollback.

Adopt those boundaries and failure semantics, but not the ESP32-S3 storage
assumptions. That platform has two internal application slots, OTA selection
metadata, signed-image support, and bootloader rollback. AZ3166 has one internal
application region and one external staging partition, and its packaged
bootloader exposes only length-and-CRC activation. The AZ3166 first release
therefore cannot provide equivalent atomic A/B rollback.

Also do not copy the ESP32-S3 authentication profile mechanically.
HomeTemperature currently has no local owner session or CSRF framework. Physical
authorization plus a signed package is the smaller defensible first boundary;
adding account management solely for OTA would be a separate product decision.

## 4. Responsibility Split

| Layer | Responsibilities |
| --- | --- |
| AZ3166 Core | Partition discovery, erase/write/read-back, bounds checking, streaming CRC16 and SHA-256, image-shape validation, signature-verification mechanism, activation metadata, typed errors, cancellation, and fault-injection seams. |
| HomeTemperature | Physical authorization, trusted public key, product/board/version policy, HTTP routes, upload ownership and deadlines, progress/status UX, coordination with cloud and discovery, and delayed reboot. |
| Host tooling | Build provenance, package construction, offline private-key signing, pre-upload inspection, upload progress, and preservation of the raw `.bin` used for ST-Link recovery. |
| Bootloader | Verify the existing CRC contract and copy the staged image into the application region. Rollback is not available in the current bootloader. |

The Core owns cryptographic implementation but not trust policy: it may verify
an image with a caller-supplied public key, while the application decides which
key, product, board, and version are acceptable. The private key exists only in
the release environment.

## 5. Security Model

A permanently available unauthenticated firmware endpoint is not acceptable,
even on a trusted LAN. CRC16 is not a security control, and TLS alone would not
prove that an uploaded image was authorized.

The proposed design requires both:

1. **Physical presence:** a deliberate board action opens a short OTA window.
   A practical first implementation can require both buttons to be held for a
   defined interval, then allow one upload for five minutes.
2. **Signed firmware:** each package is signed offline. The firmware contains
   only the public verification key; the private key must never be stored in
   this repository or on the device.

Core 3.0.0 includes Mbed TLS SHA-256, ECDSA, secp256r1, and public-key parsing,
so ECDSA P-256 with SHA-256 is the preferred initial signature scheme. Signature
policy and the trusted public key belong to the application, while the Core can
provide bounded parsing and verification helpers.

The application should reject downgrades by default. A downgrade, if needed for
recovery, requires a separate physical maintenance action. Local HTTP can remain
unencrypted for the first version because signatures protect authenticity and
integrity, but firmware contents and device metadata will be visible on the
LAN.

## 6. Package Format

Use one signed binary package rather than multipart form data or long custom
HTTP headers:

```text
+-------------------------+
| Fixed package header    |
| - magic and format      |
| - target board ID       |
| - firmware version      |
| - payload length        |
| - SHA-256 payload hash  |
| - signature metadata    |
+-------------------------+
| ECDSA signature         |
+-------------------------+
| Raw application .bin    |
+-------------------------+
```

The signature covers the canonical header fields and payload SHA-256. The
device parses the envelope while streaming but writes only the raw application
payload at offset zero of the OTA partition, preserving the existing bootloader
format.

All integers must have specified widths and byte order. Header length, signature
length, payload length, and total HTTP `Content-Length` must be checked for
overflow before erasing or writing Flash. Unknown format versions, target board
IDs, algorithms, or trailing bytes are rejected.

Before staging, validate that the payload:

- is nonempty and no larger than the runtime OTA partition capacity;
- fits the internal application partition;
- has a plausible STM32 vector table for the configured application region; and
- targets the expected AZ3166 board and firmware product.

## 7. Core Responsibilities

The Core should expose a transport-neutral session API. A representative shape
is:

```cpp
class OTAUpdateSession {
public:
    OTAResult begin(size_t expectedSize);
    OTAResult write(const uint8_t *data, size_t size);
    OTAResult finish(OTAImageInfo *result);
    void abort();
};

OTAResult OTAActivate(const OTAImageInfo &image);
```

Exact names are not prescribed, but the Core implementation should own:

- querying the application and OTA partition layouts;
- validating all offsets, additions, and lengths;
- erasing the required OTA Flash range before the first write;
- bounded sequential writes with explicit short/error results;
- streaming bootloader-compatible CRC16;
- streaming SHA-256;
- signature verification using an application-supplied trust anchor;
- optional read-back verification from external Flash;
- deterministic begin/write/finish/abort state transitions;
- exclusive ownership of one active staging session;
- clearing incomplete state without marking an image bootable;
- writing and verifying boot-table activation metadata;
- stable, typed error codes;
- progress counters that do not depend on a network transport; and
- injectable Flash operations for host-side fault tests.

The Core API must not:

- open HTTP connections or require an image URL;
- contain HomeTemperature routes, credentials, or UI;
- store a private signing key;
- reboot automatically before the caller can send a final response; or
- report success before Flash and activation metadata are verified.

The existing `OTADownloadFirmware()` can be retained as a compatibility wrapper
that feeds downloaded chunks into the new staging API.

## 8. Application Responsibilities

HomeTemperature should own:

- entering and expiring the physical OTA authorization window;
- local HTTP routes and response bodies;
- accepting only one upload at a time;
- product and board identity checks;
- the trusted public verification key;
- requiring Core signature verification and enforcing downgrade policy;
- pausing cloud uploads during staging;
- coordinating mDNS advertisement and OTA status;
- scheduling reboot only after the final response is sent; and
- reporting startup confirmation after the updated image boots.

Suggested endpoints:

| Method and path | Purpose |
| --- | --- |
| `GET /ota` | Serve a small upload page while the OTA window is open. |
| `POST /api/ota` | Stream one signed OTA package using `application/octet-stream`. |
| `GET /api/ota/status` | Return state, accepted bytes, total bytes, and last error. |
| `POST /api/ota/apply` | Activate a completely verified staged image. |
| `DELETE /api/ota` | Abort a receiving or verified-but-not-applied session. |

The first version should require `Content-Length` and reject chunked transfer,
multipart form data, ranges, resume, compression, and concurrent uploads. A
browser page can use `fetch()` to send the selected package as a raw body. A
repository-owned command-line uploader should provide the same operation for
automation.

Suggested application states:

```text
Disabled -> Armed -> Receiving -> Verifying -> Ready -> Applying
                         |             |          |
                         +-----------> Error <----+
```

Only `Ready` may transition to `Applying`. Timeout, disconnect, overflow,
signature failure, hash mismatch, Flash failure, or cancellation must leave no
bootable pending update.

## 9. HTTP Server Dependency

The current HomeTemperature `LocalWebServer` intentionally does not accept
request bodies. Local OTA therefore depends on one of these implementations:

1. extend it with a bounded streaming-body callback; or
2. first complete the proposed Core HTTP server improvements and use its
   bounded request-body stream.

The second option is preferred because OTA should not introduce another
application-owned HTTP parser. In either case, the body API must stream directly
to the Core OTA session. It must never buffer an application image in RAM.

The HTTP layer owns network deadlines and disconnect handling. The OTA session
owns Flash consistency. A dropped connection calls `abort()` and never calls
`OTAActivate()`.

## 10. Boot and Recovery Limitations

The shipped bootloader is provided as a binary, and Core 3.0.0 exposes no
rollback, boot-attempt counter, confirmed-image, or automatic recovery API.
The initial local OTA feature must therefore be documented as staged
replacement, not fail-safe A/B OTA.

Before enabling OTA outside development:

- test power loss during erase, upload, activation metadata write, and
  bootloader copy;
- confirm whether the bootloader validates CRC before modifying internal Flash;
- confirm whether an interrupted copy remains recoverable;
- verify that ST-Link can always restore the complete known-good image; and
- define an operator-visible recovery procedure.

A future bootloader design may add dual-image selection, signed manifests,
boot-attempt counters, and application health confirmation. Updating the
bootloader itself is outside the first implementation and must require a
separate recovery and manufacturing review.

## 11. Validation

### Core tests

- partition capacity and integer-overflow checks;
- erase, write, short-write, read-back, and activation failures;
- arbitrary chunk boundaries, including one-byte chunks;
- zero-length, oversized, truncated, and extra-byte packages;
- CRC16 and SHA-256 known-answer vectors;
- invalid header, board ID, version, hash, and signature;
- repeated begin, abort, finish, and activation calls;
- power-loss simulation after each persistent state transition; and
- compatibility of the URL downloader wrapper.

### HomeTemperature tests

- OTA routes are unavailable outside the physical authorization window;
- the window expires and permits only one update;
- malformed HTTP requests and unsupported transfer encodings are rejected;
- upload progress and typed failures are reported accurately;
- cloud uploads do not run during staging or activation;
- the response completes before reboot;
- mDNS and telemetry behavior recover after a cancelled upload; and
- downgrade policy requires explicit maintenance authorization.

### Hardware acceptance

- upload a signed production OTA package from a browser and the command-line
  client;
- reject corrupted, unsigned, wrong-board, oversized, and downgraded packages;
- disconnect the uploader at multiple offsets and retry;
- verify the external Flash contents before activation;
- boot the new version and report its version;
- exercise power interruption at each destructive phase; and
- recover the board through ST-Link after every intentionally failed case.

## 12. Delivery Plan

### Phase 1: Establish the bootloader baseline

1. Record the exact `.bin` format, linked address, size, SHA-256, CRC16, source
   revision, and Core version.
2. Exercise the existing staging and bootloader path on a recoverable test
   board.
3. Test interruption during staging, activation metadata persistence, and the
   bootloader copy.
4. Record which failures recover automatically and which require ST-Link.

### Phase 2: Add the Core staging engine

1. Add the transport-independent begin/write/finish/abort/activate API.
2. Add partition discovery, erase, bounds checks, CRC16, SHA-256, read-back,
   image-shape checks, typed errors, and exclusive session ownership.
3. Add Flash and clock injection with fault tests.
4. Preserve `OTADownloadFirmware()` as a compatibility wrapper.

### Phase 3: Add signed artifact tooling

1. Define and version the package envelope.
2. Add a host tool that validates the raw `.bin`, records provenance, computes
   its digest, signs the canonical metadata, and emits the OTA package.
3. Keep development and production trust roots separate.
4. Preserve the raw `.bin` and complete Flash image needed for ST-Link
   recovery.

The first OTA-capable HomeTemperature release must be installed through the
wired ST-Link workflow. That trusted bootstrap installs the public key and local
OTA implementation used to authenticate subsequent uploads.

### Phase 4: Add bounded HTTP body streaming

1. Add request-body streaming, length validation, idle and total deadlines, and
   disconnect cancellation to the selected HTTP server.
2. Reject chunked transfer, multipart bodies, ranges, and extra bytes in the
   first version.
3. Keep the HTTP worker responsive without buffering a complete image in RAM.

### Phase 5: Integrate HomeTemperature

1. Add the OTA controller, status model, and exact routes.
2. Add the physical authorization gesture and expiration window.
3. Coordinate OTA with cloud uploads, discovery, and reboot.
4. Add the command-line uploader before enabling the optional browser page.
5. Keep activation separate from upload and require the job ID plus staged
   digest.

### Phase 6: Validate and release

1. Run Core unit and hardware tests.
2. Run all HomeTemperature firmware compile suites.
3. Execute valid, invalid, interrupted, downgrade, and wrong-board uploads.
4. Exercise power interruption at every destructive phase.
5. Restore through ST-Link after every intentionally failed scenario.
6. Publish and checksum-pin the enhanced Core only after those gates pass.

### Recommended pull-request sequence

1. Core staging API and fault-injection tests.
2. Core HTTP request-body streaming and lifecycle hardening.
3. Core release and HomeTemperature version-pin update.
4. HomeTemperature OTA controller and HTTP API.
5. Host package builder and command-line uploader.
6. Browser upload page.
7. Hardware acceptance evidence and final enablement.
