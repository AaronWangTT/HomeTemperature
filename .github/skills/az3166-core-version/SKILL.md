---
name: az3166-core-version
description: 'Discover, select, upgrade, downgrade, or audit the AZ3166 Core/SDK version used by HomeTemperature. Use when listing available Board Manager versions, checking the current firmware toolchain, changing the AZ3166 version, updating package-index pins, or validating a Core migration.'
argument-hint: '[list|current|set <version>] [--upload COMx]'
user-invocable: true
disable-model-invocation: false
---

# HomeTemperature AZ3166 Core Version

Discover published AZ3166 Core versions and migrate HomeTemperature between
them without leaving source, CI, editor, provenance, or documentation pins out
of sync. Treat the maintained `azureiotdevkit_tools` `maintenance` branch as
the Board Manager catalog and the repository scripts as the build authority.

## Inputs

Interpret the invocation as one of these actions:

- `list`: list every version in the currently published maintenance index.
- `current`: report HomeTemperature's current Core version, archive checksum,
  and immutable index revision.
- `set <version>`: evaluate and, when needed, migrate to that exact numeric
  version. Do not silently select newest.
- `--upload COMx`: optional explicit authorization to upload the production
  firmware after every installation and compile gate passes.

Ask for a target only when `set` was requested without one. Ask for a port only
when upload was explicitly requested and no unambiguous port was supplied.

## Safety Rules

- Use only versions present in the maintained package index. Do not use the
  archived upstream `master` branch or infer availability from a release tag.
- Pin the raw index URL to a full 40-character commit reachable from
  `azureiotdevkit_tools` `maintenance`; never pin a branch, tag, pull-request
  head, abbreviated SHA, or mutable URL.
- Require the `validate` GitHub Actions check to have succeeded for the exact
  selected index commit. For a newly published Core, prefer its reviewed index
  merge commit after the post-merge check succeeds.
- Independently download the selected Core archive and verify both its byte
  size and SHA-256 against the index before changing HomeTemperature.
- Preserve package-manager-owned GNU Arm and OpenOCD directories shared by
  Core versions. Never repair an upgrade by manually deleting shared tools.
- Do not carry version-specific compatibility claims forward without checking
  the selected Core source or release notes.
- Do not upload until installation and all compile-only firmware suites pass.
  An upload succeeds only when OpenOCD reports `Verified OK`.
- Do not commit, push, open a pull request, or delete a branch without explicit
  user authorization. Never overwrite unrelated dirty worktree changes.

## Discover Versions

Run the bundled read-only helper from the repository root:

```powershell
& .\.github\skills\az3166-core-version\scripts\Get-Az3166CoreVersion.ps1 `
    -Action List
```

The helper resolves `maintenance` to a full SHA before downloading the index,
sorts versions semantically, and reports the current local pin alongside the
published set. If a sibling `azureiotdevkit_tools` checkout is available, fetch
it and pass its exact remote SHA:

```powershell
git -C ..\azureiotdevkit_tools fetch origin --prune
$indexRevision = git -C ..\azureiotdevkit_tools rev-parse origin/maintenance
& .\.github\skills\az3166-core-version\scripts\Get-Az3166CoreVersion.ps1 `
    -Action List `
    -IndexRevision $indexRevision
```

The helper still resolves the live `maintenance` tip through GitHub and proves
that both the selected revision and HomeTemperature's pinned revision are
reachable from it before accepting either index.

For `current`, run the helper with `-Action Current`. It audits the canonical
index URL pinned by the installer by default, while `list` and `resolve` default
to the live `maintenance` tip. Report inconsistencies between the installer,
build helper, CI cache, editor path, and documentation; do not choose one
silently.

## Resolve A Target

For `set <version>`, resolve the exact platform metadata before editing:

```powershell
& .\.github\skills\az3166-core-version\scripts\Get-Az3166CoreVersion.ps1 `
    -Action Resolve `
    -Version <version> `
    -IndexRevision <full-maintenance-sha>
```

Require exactly one matching platform entry and record:

- the full index commit and immutable raw index URL;
- archive URL, filename, byte size, and lowercase SHA-256;
- GNU Arm and OpenOCD dependency versions;
- current version, checksum, and index revision.

If the requested version and all resolved pins already match, report a no-op
and do not churn files or caches.

## Validate The Release

Download the archive to a temporary path. Compare `Length` and the lowercase
result of `Get-FileHash -Algorithm SHA256` with the resolved index metadata,
then remove the temporary file. Stop on any mismatch.

Confirm that the release tag matches the numeric package version and inspect
version-specific behavior used by HomeTemperature, including:

- `getDevkitVersion()` and any version assertions;
- `dtostrf` formatting behavior used by telemetry;
- SDK system telemetry defaults and `ENABLETRACE` behavior;
- Mbed RTOS, lwIP multicast, TLS, and bundled driver compatibility;
- compiler and OpenOCD versions declared by the platform entry.

## Apply A Version Change

Start from current `origin/main` on a dedicated consumer branch. If the current
worktree is dirty, preserve it and either continue only when it is already the
intended migration branch or use a separate worktree.

Update these coordinated surfaces from the resolved metadata:

| Surface | Required changes |
| --- | --- |
| `firmware/tools/Install-Az3166Toolchain.ps1` | Full immutable index URL, Core version, archive SHA-256, compiler version, and OpenOCD version |
| `firmware/tools/Invoke-Az3166Build.ps1` | Core version, archive SHA-256, compiler version, and OpenOCD version |
| `.github/workflows/ci.yml` | Core version and index identity in the board-package cache key |
| `.vscode/c_cpp_properties.json` | Core directory and compiler path when their versions change |
| `firmware/README.md` | Supported toolchain, immutable index URL, archive hash, install text, and relevant build notes |
| `THIRD_PARTY_NOTICES.md` | Core version, maintained tag, immutable index, release archive, hash, dependencies, and verified behavior notes |
| Root and component documentation | Exact old-version references and compatibility claims that genuinely change |

Search the whole tracked tree for the old version, old index SHA, old archive
hash, and old tool versions. Exclude generated `.tools`, `.venv`, and `.git`
content. Review every match semantically instead of replacing all text blindly.

Keep the migration minimal. Do not change Arduino IDE or ArduinoMDNS pins unless
the target Core actually requires a coordinated change, and do not combine
unrelated firmware refactors with the version update.

## Validate The Migration

Run these gates in order from the repository root:

1. Run `firmware/tools/Install-Az3166Toolchain.ps1`. Require the selected Core,
   archive stamp, compiler, OpenOCD, Arduino IDE, and ArduinoMDNS checks to pass.
2. Run the installer a second time and require an idempotent success.
3. Run `firmware/tests/run-all-tests.ps1 -Action Verify`. This compiles the
   production firmware and all repository-owned AZ3166 test sketches.
4. Parse `.vscode/c_cpp_properties.json`, check workflow diagnostics, run
   `git -c core.whitespace=cr-at-eol diff --check`, and repeat the stale-pin
   search.
5. Review the final diff and confirm it contains only the requested migration
   and any separately requested skill update.

Compile-only verification is not hardware execution. If upload was explicitly
requested and all prior gates pass, upload the production sketch with:

```powershell
& .\firmware\tools\Invoke-Az3166Build.ps1 `
    -Action Upload `
    -Sketch .\firmware\AZ3166\AZ3166.ino `
    -Port <COMx>
```

Require exit code zero and `Verified OK`. Do not claim that the eleven hardware
test sketches ran unless `run-all-tests.ps1 -Action Run -Port <COMx>` was also
explicitly requested and completed on the connected board.

## Final Report

Report the previous and selected versions, full index SHA, archive URL/size/hash,
tool dependency changes, edited surfaces, installer and compile results, and any
hardware upload/run result. State unavailable checks explicitly and distinguish
compile-only verification from execution on hardware.