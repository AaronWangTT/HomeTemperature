# Local OTA Controller

`LocalOtaController` implements the application side of the Core 3.1.2
`OTAStaging` protocol. OTA is disabled when
`config/ota_public_key.h` has no DER RFC 5480 P-256 public key. That file must
contain only the public verification key; package signing keys stay in the
release environment.

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

One controller worker is the only caller of Core staging methods. It keeps the
Core session through `Ready` and activation, while the HTTP listener remains
available for status and cancellation. `NetworkMaintenanceCoordinator` lets an
in-flight cloud upload finish, then excludes new cloud work until OTA is
terminal. Capability expiry and address-generation changes cancel the matching
session. Apply queues bounded copies of the decoded capability, peer address,
controller generation, network generation, and deadline; the worker atomically
revalidates every value immediately before `Ready` becomes `Applying`.
A successful apply remains in `ActivatedAwaitingResponse` with the OTA lease
held, so neither a new physical challenge nor cloud work can start before the
HTTP response attempt. The callback posts reboot regardless of send success;
the main loop consumes that request, releases the lease, schedules the bounded
delay, and suppresses cloud uploads while reboot is pending.
An uncertain activation remains in fatal maintenance and requires ST-Link
recovery.

Compile the deterministic controller, authorization, lease, route, upload,
apply, cancellation, expiry, and shutdown seams with:

```powershell
& .\firmware\tests\run-local-ota-controller-tests.ps1 -Action Verify
```

Compile-only testing does not prove external-Flash, boot-table, power-loss, or
bootloader recovery behavior. Complete the hardware acceptance gates in
`docs/core-local-ota-design.md` before provisioning a production key.
