# Third-Party Notices

HomeTemperature is licensed under the MIT License. The following third-party
software and data keep their own licenses and terms.

## Firmware Toolchain

The firmware builds against the maintained MXChip Azure IoT DevKit SDK / AZ3166
Arduino board package 2.0.1, based on Microsoft's final 2.0.0 release. The
package is installed separately and is not vendored in this repository.

- Maintained source: <https://github.com/AaronWangTT/devkit-sdk/tree/2.0.1>
- Upstream source: <https://github.com/microsoft/devkit-sdk/tree/2.0.0>
- Board package index: <https://raw.githubusercontent.com/AaronWangTT/azureiotdevkit_tools/d3fcd963e8e6bb0b196462c894f9b5c4816d405f/package_azureboard_index.json>
- Release archive: <https://github.com/AaronWangTT/devkit-sdk/releases/download/2.0.1/AZ3166-2.0.1.zip>
- Board archive SHA-256:
  `9908715a6d1815dbd41899b6c7cfaf65d25cfa6fcd775b096bad0d11a259e462`
- License: MIT, copyright Microsoft Corporation
- Tool dependencies declared by that package: GNU Arm Embedded Toolchain
  `5_4-2016q3` and OpenOCD `0.10.0`

The maintained release corrects `dtostrf` precision and width formatting and
makes SDK system telemetry opt-in through `ENABLETRACE=1`.

The board package contains additional third-party components. Their notices and
licenses in the installed package continue to apply.

## Firmware mDNS Responder

The firmware builds against maintained ArduinoMDNS 1.1.0, based on Georg
Kaindl's EthernetBonjour. The library is installed separately into the
repository-local Arduino sketchbook and is not vendored in this repository.

- Maintained source: <https://github.com/AaronWangTT/ArduinoMDNS/tree/1.1.0>
- Upstream source: <https://github.com/arduino-libraries/ArduinoMDNS>
- Release archive: <https://github.com/AaronWangTT/ArduinoMDNS/releases/download/1.1.0/ArduinoMDNS-1.1.0.zip>
- Release archive SHA-256:
  `f7a4c6f53d614d05aef3c6c02f6f49b4057202a42a8e40f63bdb062e99162e47`
- Library license: LGPL-3.0-or-later; the Arduino wrapper header carries its
  original LGPL-2.1-or-later notice.
- License text: <https://github.com/AaronWangTT/ArduinoMDNS/blob/1.1.0/LICENSE.txt>

The maintained release adds a borrowed custom-transport API, optional WIZnet
startup delay, responder cleanup and explicit announcements, and send-error
propagation. It also fixes used-path string allocations and query bounds,
handles ANY questions, returns NOERROR instead of NXDOMAIN for IPv6-only
questions, and uses an empty `IPAddress` for unresolved-name callbacks.
Receive-packet buffers are released at the shared cleanup exit, including
malformed packets and short reads. Original copyright and license notices are
retained in the released library.

These files retain their upstream licenses; they are not relicensed as MIT.
Firmware redistributors must comply with the LGPL, including its applicable
source and relinking requirements. Keep the corresponding library source,
notices, and reproducible firmware build instructions available with releases.

## Embedded Trust Anchor

`firmware/AZ3166/src/config/cloud_ca.h` contains the self-signed ISRG Root X1 certificate
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