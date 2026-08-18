# Third-Party Notices

HomeTemperature is licensed under the MIT License. The following third-party
software and data keep their own licenses and terms.

## Firmware Toolchain

The firmware builds against the Microsoft MXChip Azure IoT DevKit SDK / AZ3166
Arduino board package 2.0.0. The package is installed separately and is not
vendored in this repository.

- Source: <https://github.com/microsoft/devkit-sdk/tree/2.0.0>
- Board package index: <https://raw.githubusercontent.com/VSChina/azureiotdevkit_tools/d0c76e57d1ad62610aab0773ba687d55df2e4c91/package_azureboard_index.json>
- Board archive checksum declared by that index: MD5
  `4f51c0ebf4d510f28c06d203a4ce23f8`
- License: MIT, copyright Microsoft Corporation
- Tool dependencies declared by that package: GNU Arm Embedded Toolchain
  `5_4-2016q3` and OpenOCD `0.10.0`

The board package contains additional third-party components. Their notices and
licenses in the installed package continue to apply.

## Embedded Trust Anchor

`firmware/AZ3166/cloud_ca.h` contains the self-signed ISRG Root X1 certificate
used as a TLS trust anchor. Its authoritative source is the Let's Encrypt
certificate repository:

- Certificate information: <https://letsencrypt.org/certificates/>
- Official PEM: <https://letsencrypt.org/certs/isrgrootx1.pem>

Certificate trust and issuance chains can change independently of this
repository. Deployers must verify the server's current chain before building
firmware and update the trust anchor when required.

## Server Runtime

The server image installs or runs the following direct dependencies; they are
not relicensed by this project:

| Component | Version used | License | Source |
| --- | --- | --- | --- |
| Python | 3.14.7 image | PSF License | <https://www.python.org/> |
| FastAPI | 0.141.1 | MIT | <https://github.com/fastapi/fastapi> |
| Uvicorn | 0.52.3 | BSD-3-Clause | <https://github.com/encode/uvicorn> |
| httpx2 | 2.10.0 (tests) | BSD-3-Clause | <https://github.com/pydantic/httpx2> |
| Caddy | 2.10.2 image | Apache-2.0 | <https://github.com/caddyserver/caddy> |

Transitive Python packages and container image contents retain their upstream
licenses. Release artifacts should include an SBOM or equivalent dependency
inventory generated from the final images.