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

Open `http://az3166.local/ota` for the optional command-page UI. It is a
same-origin wrapper around the APIs below: firmware bytes still flow through
the signed streaming endpoint, and apply still requires the staged generation
and digest. The page does not contain or replace signing keys.

The routes are `POST /api/ota`, `GET /api/ota/status`,
`POST /api/ota/apply`, and `DELETE /api/ota`. Uploads
must use one `Content-Length` and `application/octet-stream`; transfer encoding,
multipart upload, ranges, and resume are unsupported.

There is intentionally no button gesture, OLED challenge, password, session,
or capability token. Any client on the trusted LAN may invoke the endpoints,
but Core accepts only a package that passes the configured signature, identity,
version, bounds, digest, vector-table, and full Flash read-back checks.

Successful upload and `Ready` status responses include the controller
generation and staged payload digest. Apply requires the exact canonical JSON
generation and digest selected by the operator; the device compares both with
its retained Core session before activation.
`GET /api/version` is a read-only version probe used by the command-line
client after reboot.

One controller worker is the only caller of Core staging methods. It keeps the
Core session through `Ready` and activation, while the HTTP listener remains
available for status and cancellation. `NetworkMaintenanceCoordinator` lets an
in-flight cloud upload finish, then excludes new cloud work until OTA is
terminal. Address-generation changes cancel the matching session. Apply returns
`202 Accepted` first; the response-attempt callback then releases the worker to
persist boot metadata. The client confirms completion through `/api/version`
after the ensuing reset and bootloader copy.
An uncertain activation remains in fatal maintenance and requires ST-Link
recovery.

Successful upload and `Ready` status responses include the Core staging
generation and lowercase payload SHA-256 digest. The command-line client
requires both to match its locally verified package and sends both in the
separate apply request.

Build, verify, upload, activate, and confirm packages with the repository-owned
host tool documented in
[`tools/ota/README.md`](../../../../tools/ota/README.md).

Compile the deterministic controller, lease, route, upload, apply,
cancellation, network-change, and shutdown seams with:

```powershell
& .\firmware\tests\run-local-ota-controller-tests.ps1 -Action Verify
```

Compile-only testing does not prove external-Flash, boot-table, power-loss, or
bootloader recovery behavior. Complete the hardware acceptance gates in
`docs/core-local-ota-design.md` before provisioning a production key.
