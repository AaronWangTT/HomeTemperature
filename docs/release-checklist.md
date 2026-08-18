# Public Release Checklist

## Before the Initial Commit

- [ ] Review `git status --short --ignored` and confirm that local firmware
      configuration, `.env`, databases, WAL/SHM files, backups, and caches are
      ignored.
- [ ] Scan every candidate tracked file and the final archive for secrets and
      deployment-specific identifiers.
- [ ] Confirm that no earlier repository, patch, chat attachment, or archive
      exposed a device key or dashboard password. Rotate any credential whose
      confidentiality is uncertain.
- [ ] Run the server tests and the production plus ten-suite firmware Verify.
- [ ] Review `LICENSE` and `THIRD_PARTY_NOTICES.md`.

## GitHub Repository Settings

- [ ] Set the default branch to `main`.
- [ ] Require the `Server`, `Firmware compile`, `Gitleaks`, and `Python
      analysis` checks before merging.
- [ ] Require pull requests and prevent force pushes to `main`.
- [ ] Enable secret scanning and push protection.
- [ ] Enable Dependabot alerts, security updates, and private vulnerability
      reporting.
- [ ] Avoid storing production credentials as Actions secrets; CI does not
      require them.

## Release Artifact

- [ ] Build from a clean clone with no local deployment headers.
- [ ] Generate checksums and an SBOM for distributed container images or
      binaries.
- [ ] Confirm the release archive excludes all ignored runtime files.
- [ ] Document the tested Core, server image, and dependency versions.
- [ ] Run hardware tests separately and record the board, port, OpenOCD
      `Verified OK`, firmware resource usage, and production restoration.