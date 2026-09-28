# Local OTA Hardware Acceptance

This record tracks physical AZ3166 validation of the signed local OTA path.
Software tests and compile checks do not replace the destructive and
power-interruption cases below.

## 2026-09-28 partial run

Environment:

- MXCHIP AZ3166 connected through its onboard ST-Link virtual COM port
  (`COM3` for this run); the exact probe serial was supplied to OpenOCD and is
  retained only in private test evidence;
- AZ3166 Core 3.1.3;
- OpenOCD 0.10.0;
- ephemeral P-256 test key stored outside the repository;
- test key ID added to the local allowlist only for the run and removed
  afterward.

Completed evidence:

1. Read all 1,048,576 bytes of internal Flash before modification.
2. Recorded backup SHA-256
   `ca7852467f20858e91024ecc7b0638508afd2415286aecf8d3c113c00ca918dd`.
3. Built a fail-closed OTA-capable `0.0.1` bootstrap whose final binary
   contained the expected product, board, source revision, layout, and test-key
   identifier.
4. Flashed that bootstrap through the explicitly selected ST-Link. OpenOCD
   reported `Verified OK`.
5. Verified `GET /api/version` returned `0.0.1`.
6. Verified `GET /ota` returned HTTP 200 with the embedded page.
7. Uploaded a valid signed `0.0.2` package through the LAN and verified the
   staged status reached `Ready` with the expected digest.
8. Applied the staged image through the asynchronous endpoint. The request was
   accepted before activation began, and `GET /api/version` reported `0.0.2`
   after approximately 89.87 seconds.
9. Restored the complete pre-test internal Flash image. OpenOCD reported
   `Verified OK`.
10. Read back all 1,048,576 restored bytes; their SHA-256 exactly matched the
   pre-test backup.

The first upload attempt exposed a Windows/OpenOCD integration defect before
Flash programming began: Tcl interpreted backslashes in the binary path as
escape sequences. The build helper now normalizes the path to forward slashes,
and the subsequent verified upload succeeded.

## Remaining physical gates

The following cases remain incomplete and must not be inferred from the
partial run:

- repeat the valid signed upgrade using firmware and package artifacts rebuilt
  from the final merged HomeTemperature revision and the immutable Core 3.1.3
  archive;
- reject unsigned, corrupt, wrong-key, wrong-board, downgrade, and oversized
  packages on the physical board;
- disconnect at representative package offsets and verify recovery;
- interrupt power during staging erase/write, activation metadata persistence,
  and bootloader copy;
- verify automatic recovery or ST-Link recovery for every interrupted case;
- measure HTTP worker stack high-water marks while serving `/ota`, uploading,
  polling status, and applying;
- restore and read back the intended final production firmware after the full
  run.

Do not provision a production trust key or describe local OTA as
hardware-accepted until every remaining gate has recorded evidence.
