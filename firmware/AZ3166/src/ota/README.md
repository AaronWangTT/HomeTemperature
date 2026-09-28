# Local OTA Controller

`LocalOtaController` implements the application side of the Core 3.1.2
`OTAStaging` protocol. OTA is disabled when
`config/ota_public_key.h` has no DER RFC 5480 P-256 public key. That file must
contain only the public verification key; package signing keys stay in the
release environment.

OTA is also disabled unless the build supplies
`HOME_TEMPERATURE_FIRMWARE_VERSION` as the canonical `MAJOR.MINOR.PATCH`
version embedded in the exact image. There is no default version because a
stale fallback would permit replay or downgrade of signed firmware.
The production sketch uses the repository OTA linker to retain its 256-byte
`AZOTA001` descriptor at image offset `0x200`. Generate the public release
configuration and invoke the build through the host-tool workflow in
`tools/ota/README.md`; a normal build emits a fail-closed descriptor that
cannot be signed by a configured key.

Hold both device buttons for two seconds to display an eight-lowercase-hex
challenge for 30 seconds. A successful `POST /api/ota/session` claim returns a
single-use, source- and network-generation-bound 128-bit capability valid for
five minutes. Mutating and detailed-status routes require exactly:

```text
Authorization: OTA <32 lowercase hexadecimal characters>
```

The routes are `POST /api/ota/session`, `POST /api/ota`,
`GET /api/ota/status`, `POST /api/ota/apply`, and `DELETE /api/ota`. Uploads
must use one `Content-Length` and `application/octet-stream`; transfer encoding,
multipart upload, ranges, and resume are unsupported.

Successful upload and `Ready` status responses include the controller
generation. Apply is bodyless and activates only the controller's retained
Core session. The host client independently verifies the selected package and
requires its supplied digest and live Ready generation to match before apply.
`GET /api/version` is a read-only, unauthenticated version probe used by the
command-line client after reboot; it does not expose a capability or enable
OTA.

One controller worker is the only caller of Core staging methods. It keeps the
Core session through `Ready` and activation, while the HTTP listener remains
available for status and cancellation. `NetworkMaintenanceCoordinator` lets an
in-flight cloud upload finish, then excludes new cloud work until OTA is
terminal. Capability expiry and address-generation changes cancel the matching
session. A successful apply posts reboot only after the HTTP response attempt.
An uncertain activation remains in fatal maintenance and requires ST-Link
recovery.

Successful upload and `Ready` status responses include the Core staging
generation and lowercase payload SHA-256 digest. The command-line client
requires both to match its locally verified package before it sends the
separate bodyless apply request.

Build, verify, upload, activate, and confirm packages with the repository-owned
host tool documented in
[`tools/ota/README.md`](../../../../tools/ota/README.md).

Compile the deterministic controller, authorization, lease, route, upload,
apply, cancellation, expiry, and shutdown seams with:

```powershell
& .\firmware\tests\run-local-ota-controller-tests.ps1 -Action Verify
```

Compile-only testing does not prove external-Flash, boot-table, power-loss, or
bootloader recovery behavior. Complete the hardware acceptance gates in
`docs/core-local-ota-design.md` before provisioning a production key.
