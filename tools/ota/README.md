# Local OTA Host Tooling

The Python CLI builds, verifies, uploads, and explicitly applies Core 3.1.2
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

The raw `.bin` must already contain its retained 256-byte `AZOTA001`
compatibility descriptor at offset `0x200`. Its canonical product, board,
version, source commit, application layout, package format, and key identifier
must match the CLI arguments and the public key derived from the private key.
Keep this exact raw `.bin` for ST-Link recovery.

Build a signed package:

```powershell
python .\tools\ota\ota_cli.py build `
  --image .\artifacts\HomeTemperature.bin `
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
signature.

## Trusted-LAN upload

Hold both device buttons until the eight-character challenge appears, then
claim it:

```powershell
$capability = python .\tools\ota\ota_cli.py claim `
  --base-url http://az3166.local `
  --challenge 1234abcd
```

The capability is sensitive and remains only in process memory when passed
with `--capability`. As an alternative, set
`HOME_TEMPERATURE_OTA_CAPABILITY`; do not place it in a URL or log.

Upload and retain the returned generation and digest:

```powershell
python .\tools\ota\ota_cli.py upload `
  --base-url http://az3166.local `
  --capability $capability `
  --package .\artifacts\HomeTemperature.azpkg `
  --public-key C:\secure\ota-public-key.der
```

Status is a separate authorized request:

```powershell
python .\tools\ota\ota_cli.py status `
  --base-url http://az3166.local `
  --capability $capability
```

Apply is deliberately separate. Supply the generation and digest returned by
upload; the CLI verifies the package again and requires the live status to be
`Ready` with that generation before it sends the bodyless apply request:

```powershell
python .\tools\ota\ota_cli.py apply `
  --base-url http://az3166.local `
  --capability $capability `
  --package .\artifacts\HomeTemperature.azpkg `
  --public-key C:\secure\ota-public-key.der `
  --generation 7 `
  --digest 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
```

After apply, the CLI polls `GET /api/version` until it observes the package
version or the reboot timeout expires. Use `--verify-url` and
`--version-field` only when integrating with a different JSON version
endpoint.

Run the host tests, which generate only ephemeral keys:

```powershell
python -m unittest discover -s .\tools\ota\tests -v
python -m compileall -q .\tools\ota
```
