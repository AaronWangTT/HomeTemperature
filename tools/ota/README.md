# Local OTA Host Tooling

The Python CLI builds, verifies, uploads, and explicitly applies Core 3.1.3
`AZPKG001` packages. It has no browser UI and stores no credentials.

Install the one pinned cryptographic dependency in an isolated environment:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r .\tools\ota\requirements.txt
```

## Keys and release image

Generate and protect a P-256 signing key outside this repository. Pass the
private-key path explicitly with `--private-key` or set
`HOME_TEMPERATURE_OTA_PRIVATE_KEY` to that path. The CLI never prints private
key material. Do not commit the key.

Production operations are fail-closed. Add the lowercase SHA-256 of the
canonical public-key DER to the reviewed
`tools/ota/production_key_ids.py` allowlist before generating build
configuration, signing, uploading, or applying. The allowlist is intentionally
empty until the production trust anchor is approved.

The raw `.bin` must already contain its retained 256-byte `AZOTA001`
compatibility descriptor at offset `0x200`. Its canonical product, board,
version, source commit, application layout, package format, and key identifier
must match the CLI arguments and the public key derived from the private key.
Keep this exact raw `.bin` for ST-Link recovery.

Generate a public build configuration outside the checkout, then compile the
production sketch with the repository OTA linker. The generated header contains
only the public verification key and release metadata:

```powershell
python .\tools\ota\ota_cli.py build-config `
  --public-key C:\secure\ota-public-key.der `
  --output C:\secure\ota-build-config.h `
  --version 1.2.3 `
  --source 0123456789abcdef0123456789abcdef01234567

.\firmware\tools\Invoke-Az3166Build.ps1 `
  -Action Verify `
  -Sketch .\firmware\AZ3166\AZ3166.ino `
  -BuildPath .\artifacts\ota-build `
  -OtaBuildConfig C:\secure\ota-build-config.h
```

The build fails if the retained descriptor is absent or not exactly 256 bytes
at image offset `0x200`. The resulting raw image is
`artifacts\ota-build\AZ3166.ino.bin`.
For production `-Action Upload`, the wrapper performs this compile and
descriptor validation before invoking OpenOCD on that exact validated binary;
an invalid image is never passed to the flashing command. Production wired
upload generates the build configuration itself, revalidates the final image
against the reviewed key allowlist, and selects the exact ST-Link probe by its
24-hex-character serial number:

```powershell
.\firmware\tools\Invoke-Az3166Build.ps1 `
  -Action Upload `
  -Sketch .\firmware\AZ3166\AZ3166.ino `
  -StLinkSerial 00112233445566778899AABB `
  -OtaPublicKey C:\secure\ota-public-key.der `
  -FirmwareVersion 1.2.3 `
  -SourceCommit 0123456789abcdef0123456789abcdef01234567
```

Do not pass `-Port` or a hand-written `-OtaBuildConfig` to production Upload.
The wrapper generates the configuration from the public key and release
metadata, requires its descriptor key ID to be allowlisted, and uses
`-StLinkSerial` to select the exact probe.

Hardware test runners use the separate `Restore` action after an on-board
suite. It compiles and validates the repository's default fail-closed,
OTA-disabled production firmware before restoring it through the attached
ST-Link; it cannot provision or enable an OTA trust key.

Build a signed package:

```powershell
python .\tools\ota\ota_cli.py build `
  --image .\artifacts\ota-build\AZ3166.ino.bin `
  --output .\artifacts\HomeTemperature.azpkg `
  --private-key C:\secure\ota-signing-key.pem `
  --version 1.2.3 `
  --source 0123456789abcdef0123456789abcdef01234567
```

Verify it independently with the provisioned DER RFC 5480 P-256
SubjectPublicKeyInfo:

```powershell
python .\tools\ota\ota_cli.py verify `
  --package .\artifacts\HomeTemperature.azpkg `
  --public-key C:\secure\ota-public-key.der `
  --version 1.2.3 `
  --source 0123456789abcdef0123456789abcdef01234567
```

The verifier checks package lengths and fixed fields, canonical descriptor
encoding, application address and capacity, key ID, vector table, embedded
descriptor equality, payload SHA-256, and the raw big-endian P-256 `r || s`
signature. Standalone verification accepts the explicitly supplied inspection
key; it cannot generate, upload, or activate a package.

## Trusted-LAN upload

Upload and retain the returned generation and digest:

```powershell
python .\tools\ota\ota_cli.py upload `
  --base-url http://az3166.local `
  --package .\artifacts\HomeTemperature.azpkg `
  --public-key C:\secure\ota-public-key.der
```

Upload waits up to 120 seconds by default because the device performs bounded
Flash erase, write, and full read-back verification before returning `Ready`.
Use `--timeout` only when hardware measurements justify another bound.

Status is a separate request:

```powershell
python .\tools\ota\ota_cli.py status `
  --base-url http://az3166.local
```

Apply is deliberately separate. Supply the generation and digest returned by
upload; the CLI verifies the package again and requires the live status to be
`Ready` with that generation and digest before it sends both values in the
apply request:

```powershell
python .\tools\ota\ota_cli.py apply `
  --base-url http://az3166.local `
  --package .\artifacts\HomeTemperature.azpkg `
  --public-key C:\secure\ota-public-key.der `
  --generation 7 `
  --digest 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
```

After apply, the CLI polls `GET /api/version` until it observes the package
version or the reboot timeout expires. Use `--verify-url` and
`--version-field` only when integrating with a different JSON version
endpoint.

These endpoints intentionally have no challenge, password, or capability
exchange. Use them only on a trusted LAN. Package signatures, product/board
identity, version policy, payload digest, Flash read-back, and generation/digest
activation binding still determine whether firmware can be installed.

Run the host tests, which generate only ephemeral keys:

```powershell
python -m unittest discover -s .\tools\ota\tests -v
python -m compileall -q .\tools\ota
```
