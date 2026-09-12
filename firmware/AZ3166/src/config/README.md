# Application Configuration

Keep application constants, deployment inputs, and TLS trust material separate
from reusable component implementations. This folder is configuration for the
HomeTemperature application, not a runtime configuration service or a generic
cloud transport library.

## Files and Responsibilities

| File | Purpose |
| --- | --- |
| [AppConfig.h](AppConfig.h) | Application ports, routes, timing, payload limits, measurement ranges, and watchdog settings. |
| [cloud_config.h](cloud_config.h) | Load optional sibling deployment/secrets headers and supply fallback macros. |
| [cloud_ca.h](cloud_ca.h) | Public TLS trust certificate used by the configured HTTPS client; not a private key. |
| [cloud_deployment.example.h](cloud_deployment.example.h) | Tracked template defining `HOME_TEMPERATURE_CLOUD_ENDPOINT`. |
| [cloud_secrets.example.h](cloud_secrets.example.h) | Tracked template defining the placeholder `CLOUD_DEVICE_API_KEY`. |

The optional local copies are named `cloud_deployment.h` and `cloud_secrets.h`.
They are ignored by Git at their paths in this folder. They are not distributed
as configuration defaults, checked into the repository, or copied into focused
test sketches.

## Configuration Flow

```text
Ignored local overrides, when present
    -> cloud_config.h defaults for missing values
    -> AppConfig constants
    -> constructor arguments in the application sketch
```

Keep the loader before application constants where deployment overrides are
needed:

```cpp
#include "src/config/cloud_config.h"
#include "src/config/AppConfig.h"
#include "src/config/cloud_ca.h"
```

The loader uses `__has_include` when supported by the toolchain to find optional
headers alongside itself. Without local overrides, the endpoint defaults to
`telemetry.example.com` and the placeholder key leaves cloud upload disabled.
Wi-Fi provisioning is handled separately by the board package.

Preprocessor definitions are local to each translation unit. Including the
loader in the sketch does not automatically define those macros in every source
file. Supply effective deployment values through component constructors; do not
make a reusable component reach into private application configuration.

## Set Up a Deployment

From the repository root, create local copies only if they do not already exist:

```powershell
Copy-Item firmware/AZ3166/src/config/cloud_deployment.example.h firmware/AZ3166/src/config/cloud_deployment.h
Copy-Item firmware/AZ3166/src/config/cloud_secrets.example.h firmware/AZ3166/src/config/cloud_secrets.h
```

Set the endpoint and a random device key of at least 32 characters in those
local copies. Match the server's authentication configuration and verify that
the public CA header contains a suitable trust anchor for its certificate chain.
Never place an actual key in an example, README, test fixture, log, command line,
or issue report. A successful key-format check is not proof of server acceptance.

For checkouts predating the configuration-folder move, relocate existing private
overrides from the sketch root instead of overwriting them with templates.
Only the new paths are ignored; remove or migrate legacy copies before staging
changes. Ignore rules do not protect a file that is already tracked.

These path-only checks can help verify local protection:

```powershell
git check-ignore -- firmware/AZ3166/src/config/cloud_deployment.h firmware/AZ3166/src/config/cloud_secrets.h
git ls-files -- firmware/AZ3166/src/config/cloud_deployment.h firmware/AZ3166/src/config/cloud_secrets.h
```

The first command should list both paths; the second should produce no paths.
The repository's [.gitignore](../../../../.gitignore) is the source of those rules.

## Reuse and Adaptation

The generic [HTTP service](../http/README.md), [discovery service](../discovery/README.md),
and [cloud transport](../cloud/README.md) accept their configuration from callers.
Another application can construct them with its own values without copying
HomeTemperature's route names or deployment headers.

Some adapters deliberately share this application's constants. For example,
sensor validation and JSON field names follow the ingestion contract, while
`TelemetryUploader` uses the configured payload capacity. Treat changes to
those values as behavior changes, not simply cosmetic configuration edits.

| Setting group | Current application choices |
| --- | --- |
| Local service | Port 80, `/api/telemetry`, `az3166.local`, matching HTTP service/TXT metadata. |
| Scheduling | Five-minute normal cloud interval and 15-second retry interval. |
| Connectivity | One-second status sampling, 5-to-60-second Wi-Fi backoff, 60-second NTP retry. |
| Input and recovery | 50 ms button debounce and 30-second watchdog. |
| Telemetry | 160-byte upload buffer and explicitly bounded measurement ranges. |

Keep related values consistent: the advertised port must match the actual
listener, and encoded TXT lengths must match their content. The default `.local`
name assumes one device with that label per LAN. No runtime settings editor,
provisioning portal, automatic key rotation, or certificate refresh is provided.

## Validation and References

Focused suites stage `config/AppConfig.h` individually when needed. They do not
copy the whole configuration directory or private overrides. A change to a
shared constant should be checked against its owning component's tests; a real
endpoint or trust-anchor change additionally needs an authorized TLS/upload
integration check.

See [firmware setup](../../../README.md#configuration), the
[firmware design](../../../../docs/firmware-design.md#14-configuration-and-credentials),
and [trust-anchor notices](../../../../THIRD_PARTY_NOTICES.md#embedded-trust-anchor).