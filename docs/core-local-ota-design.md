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
| AZ3166 Core | Package-envelope parsing, partition discovery, erase/write/read-back, bounds checking, streaming CRC16 and SHA-256, image-shape validation, signature-verification mechanism, activation metadata, typed errors, cancellation, and fault-injection seams. |
| HomeTemperature | Physical authorization, trusted public key, product/board/version policy, HTTP routes, upload ownership and deadlines, progress/status UX, coordination with cloud and discovery, and delayed reboot. |
| Host tooling | Build provenance, package construction, offline private-key signing, pre-upload inspection, upload progress, and preservation of the raw `.bin` used for ST-Link recovery. |
| Bootloader | Consume the existing length-and-CRC boot-table contract and copy the staged image into the application region. Whether it validates CRC before modifying internal Flash remains a hardware-test gate. Rollback is not available in the current bootloader. |

The Core owns cryptographic implementation but not trust policy: it may verify
an image with a caller-supplied public key, while the application decides which
key, product, board, and version are acceptable. The private key exists only in
the release environment. Format version 1 accepts the public key only as a
DER-encoded RFC 5480 SubjectPublicKeyInfo for the named secp256r1 curve. The
32-byte signing-key identifier is SHA-256 over those exact DER bytes. The host
tool and Core reject other key encodings and require the descriptor identifier
to equal the digest of the configured trust anchor before admission.

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

Core 3.0.0 includes the Mbed TLS sources for SHA-256, ECDSA, secp256r1, and
public-key parsing, but its effective `mbed_config.h` enables only SHA-256.
ECDSA P-256 with SHA-256 is the preferred initial signature scheme only after a
future Core release enables and links the required ECP, ECDSA, bignum, ASN.1,
and public-key parsing modules. Core CI must compile and execute a
known-answer signature verification test and record the resulting flash and RAM
cost. Signature policy and the trusted public key belong to the application,
while the Core provides bounded parsing and verification.

Firmware versions use exactly three decimal components,
`MAJOR.MINOR.PATCH`. Each component is in the range 0 through 65535, has no
leading zero unless it is zero, and is compared numerically and
lexicographically as a three-element tuple. A normal OTA session rejects both
equal and lower versions, preventing replay of an already installed release.
The source commit identifies provenance but does not participate in ordering.
A downgrade, if needed for recovery, requires a separate physical maintenance
action that explicitly changes the admission policy for one session.

Local HTTP can remain unencrypted for the first version because signatures
protect authenticity and integrity, but firmware contents and device metadata
will be visible on the LAN.

## 6. Package Format

Use one signed binary package rather than multipart form data or long custom
HTTP headers:

```text
+------------------------------+
| 64-byte envelope prefix      |
| - magic and format           |
| - payload length and SHA-256 |
| - signature algorithm/size   |
+------------------------------+
| 256-byte descriptor copy     |
+------------------------------+
| 64-byte ECDSA signature      |
| raw P-256 r || s             |
+------------------------------+
| Raw application .bin         |
+------------------------------+
```

The fixed package header is 320 bytes: a 64-byte envelope prefix followed by an
exact copy of the firmware's 256-byte compatibility descriptor. The signature
covers all 320 header bytes. Format version 1 defines the prefix as:

| Offset | Size | Field |
| ---: | ---: | --- |
| `0` | 8 | Magic `AZPKG001` |
| `8` | 2 | Package format version, value 1 |
| `10` | 2 | Total header size, value 320 |
| `12` | 2 | Signature algorithm, value 1 for ECDSA P-256/SHA-256 |
| `14` | 2 | Signature size, value 64 |
| `16` | 4 | Raw application payload length |
| `20` | 32 | SHA-256 of the complete raw application payload |
| `52` | 12 | Reserved, all zero |
| `64` | 256 | Exact copy of the compatibility descriptor |

All integers are unsigned and little endian. The signature begins at package
offset 320, and the raw application begins at package offset 384. The parser
must validate the magic, all fixed sizes and identifiers, reserved bytes, exact
package-length equation, descriptor encoding, runtime partition values, signing
key identifier, and header signature before invoking application admission or
erasing the staging partition. Only checks that depend on payload bytes are
deferred until streaming and read-back.

The application image places its compatibility descriptor at raw-image offset
`0x200` in a dedicated, retained linker section. Its versioned layout is:

| Offset | Size | Field |
| ---: | ---: | --- |
| `0` | 8 | Magic `AZOTA001` |
| `8` | 2 | Descriptor format version |
| `10` | 2 | Descriptor size, exactly 256 |
| `12` | 4 | Security profile |
| `16` | 32 | NUL-terminated product ID |
| `48` | 32 | NUL-terminated board ID |
| `80` | 32 | NUL-terminated firmware version |
| `112` | 40 | Lowercase hexadecimal source commit, fixed width without NUL |
| `152` | 4 | Application address |
| `156` | 4 | Application capacity |
| `160` | 4 | Package-format version |
| `164` | 32 | Trusted signing-key SHA-256 identifier |
| `196` | 60 | Zero-filled reserved bytes |

The product, board, and firmware-version strings must contain a NUL within their
fields, and all bytes after it must be zero. The source commit is instead
exactly 40 lowercase hexadecimal bytes without a terminator. The descriptor and
package format versions are independent and both are checked. The build fails
unless the linker map places exactly one 256-byte descriptor at raw-image offset
`0x200`.

After read-back, the Core extracts the embedded descriptor and requires it to be
byte-for-byte identical to the descriptor copied into the signed package
header. This prevents a correctly signed image for another product using the
same signing authority from being accepted solely because it targets AZ3166
hardware.

The device parses the envelope while streaming but writes only the raw
application payload at offset zero of the OTA partition, preserving the existing
bootloader format.

The first format uses an exactly 320-byte header and an exactly 64-byte raw
ECDSA P-256 signature containing `r || s`. Each scalar is an unsigned,
big-endian, exactly 32-byte value, left-padded with zero bytes when necessary.
No variable-sized header or signature allocation is permitted. Payload length
and total HTTP `Content-Length` must be checked for overflow and must satisfy:

```text
package length = 320 + 64 + payload length
```

Unknown format versions, target board IDs, algorithms, nonzero reserved bytes,
inconsistent lengths, or impossible payload declarations are rejected before
Flash erase. Payload hash mismatches, embedded descriptor mismatches, and
trailing bytes are rejected before staging completes. Tests must include
oversized and malformed header, signature, and payload declarations.

Before staging, validate that the payload:

- is nonempty and no larger than the runtime OTA partition capacity;
- fits the internal application partition;
- declares an application address and capacity exactly equal to the values
  returned for `MICO_PARTITION_APPLICATION`;
- has a plausible STM32 vector table for the configured application region; and
- contains a valid embedded compatibility descriptor matching the signed
  product, board, version, and format-generation metadata.

## 7. Core Responsibilities

The Core should expose a transport-neutral package session API. It consumes the
complete wire package, parses the bounded envelope and signature before writing
only payload bytes to the OTA partition, and asks the application to approve
the parsed metadata before erase begins. A representative shape is:

```cpp
typedef OTAResult (*OTAAdmissionCallback)(
    const OTAPackageMetadata *metadata,
    void *context);

class OTAUpdateSession {
public:
    OTAResult begin(
        size_t packageSize,
        const uint8_t *trustedPublicKeyDer,
        size_t trustedPublicKeyDerSize,
        OTAAdmissionCallback admit,
        void *context);
    OTAResult writePackage(const uint8_t *data, size_t size);
    OTAResult finish(OTAStagedImageInfo *result);
    OTAResult activate(
        uint32_t sessionId,
        const uint8_t expectedSha256[32]);
    void abort();
};
```

Exact names are not prescribed, but the Core implementation should own:

- incrementally parsing a fixed-size envelope across arbitrary input chunks;
- buffering only the bounded header and signature, never the complete image;
- preserving payload bytes that share the input chunk completing the signature;
- verifying the 320-byte header signature with the supplied trust anchor before
  exposing authenticated metadata to the admission callback or erasing Flash;
- invoking the admission callback only after signature verification and before
  Flash erase;
- rejecting trailing bytes or a package length inconsistent with its envelope;
- querying the application and OTA partition layouts;
- requiring the signed application address and capacity to match the runtime
  application partition exactly, then validating all offsets, additions, and
  lengths against both application and staging partitions;
- erasing the required OTA Flash range before the first write;
- bounded sequential writes with explicit short/error results;
- streaming bootloader-compatible CRC16;
- streaming SHA-256;
- signature verification using the application-supplied trust anchor;
- mandatory full read-back from external Flash with independently recomputed
  CRC16 and SHA-256 before the image can become staged;
- extraction and exact comparison of the embedded firmware descriptor against
  the authenticated package metadata;
- deterministic begin/write/finish/abort state transitions;
- exclusive ownership of one active staging session;
- clearing incomplete state without marking an image bootable;
- assigning a nonzero session generation and retaining the current staged
  generation and SHA-256 internally;
- accepting activation only when the supplied generation and digest match the
  Core's current `Ready` state, then consuming that capability so it cannot be
  replayed;
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

The existing `OTADownloadFirmware()` accepts a raw application image, not this
signed package. Preserve it only as a separately named legacy raw-image path so
existing sketches retain their behavior; deprecate it and document that it
does not provide the package authenticity guarantees above. It must not call
the signed package-session API or be used by HomeTemperature. A new URL-based
signed-package helper, if needed, can feed complete package bytes into the same
session API used by local upload.

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

`LocalOtaController` owns the complete application OTA state behind one mutex.
Button handling and the main loop may only publish authorization or lifecycle
commands and read snapshots. HTTP handlers may only reserve a job, transfer an
upload request to its worker, query a snapshot, or enqueue cancel/apply
commands. The dedicated upload worker is the sole caller of the Core package
session and the sole writer to OTA Flash.

Cancellation sets a generation-bound flag under the controller mutex; only the
upload worker observes that flag and calls `abort()`. Activation is accepted
only after that worker has completed and published `Ready`. Cloud scheduling
reads the same synchronized snapshot and skips new uploads while OTA is busy.
No mutex is held during socket I/O, Flash operations, hashing, signature
verification, callbacks, or reboot. Reboot is posted to the main loop only
after activation succeeds and the HTTP response has completed.

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
bootable pending update. The job ID and digest returned by `finish()` identify
only the Core's current in-memory `Ready` state; they are not caller-authoritative
image metadata. Activation must fail after cancellation, another successful
staging session, activation, or reboot.

## 9. HTTP Server Dependency

The current HomeTemperature `LocalWebServer` intentionally does not accept
request bodies and synchronously serves one client on its only worker. Local OTA
therefore requires the proposed Core HTTP server improvements rather than a
simple body callback added to the current server.

The enhanced Core HTTP server must allow an accepted OTA request and its socket
to be transferred to one dedicated upload worker. The listener then resumes
accepting short status and cancellation requests while the upload proceeds.
Only one detached upload is allowed, and all other request handlers remain
bounded. Status polling is read-only; cancellation signals the upload owner
rather than closing or writing its socket from another thread.

The body API streams directly to the Core OTA session and never buffers an
application image in RAM.

The parser-to-body transition must preserve bytes already received after the
header terminator. `LocalWebServer::readRequest()` currently reads blocks and
returns immediately at the blank line, so a POST body arriving in that same
socket read would otherwise be discarded. The streaming API must deliver those
prefetched bytes first, then continue reading the socket under the same length
and deadline accounting.

The HTTP layer owns network deadlines and disconnect handling. The OTA session
owns Flash consistency. A dropped connection calls `abort()` and never calls
the session's `activate()` method.

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
- invalid header, product, board ID, version, hash, and signature;
- disagreement between signed package metadata and the embedded firmware
  descriptor;
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
3. Add authenticated pre-erase admission and session-bound activation.
4. Add Flash and clock injection with fault tests.
5. Preserve `OTADownloadFirmware()` as a deprecated legacy raw-image path,
   separate from the signed package-session API.

### Phase 3: Add signed artifact tooling

1. Define and version the package envelope and 256-byte embedded descriptor.
2. Add a retained linker section at application-image offset `0x200`, populate
   it from build-generated product, board, version, source, layout, and signing
   key identifiers, and fail the build when the map or extracted bytes differ.
3. Add compile and host tests that locate and parse the descriptor in the final
   `.bin`, not only in an object file.
4. Add a host tool that validates the raw `.bin`, verifies its embedded
   descriptor, records provenance, computes its digest, copies the descriptor
   into the package header, signs that header, and emits the OTA package.
5. Keep development and production trust roots separate.
6. Preserve the raw `.bin` and complete Flash image needed for ST-Link
   recovery.

The first OTA-capable HomeTemperature release must be installed through the
wired ST-Link workflow. That trusted bootstrap installs the public key and local
OTA implementation used to authenticate subsequent uploads.

### Phase 4: Add bounded HTTP body streaming

1. Add request-body streaming, length validation, idle and total deadlines, and
   disconnect cancellation to the selected HTTP server.
2. Add explicit request/socket ownership transfer to a dedicated upload worker
   so the listener remains available for status and cancellation.
3. Reject chunked transfer, multipart bodies, ranges, and extra bytes in the
   first version.
4. Keep the HTTP listener responsive without buffering a complete image in RAM.

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
