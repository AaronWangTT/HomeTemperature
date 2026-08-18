---
name: production-deployment
description: 'Safely deploy or upgrade the HomeTemperature server in production over SSH. Use for production release, rollout, deployment preflight, backup, acceptance validation, or rollback of the Docker Compose gateway.'
argument-hint: '[revision] [ssh-target] [remote-project-directory] [dry-run]'
user-invocable: true
disable-model-invocation: false
---

# HomeTemperature Production Deployment

Deploy one immutable server revision with auditable backups, provenance, health
checks, and a prepared rollback. Treat [server/DEPLOY.md](../../../server/DEPLOY.md)
as the controlling runbook and the repository scripts as the implementation of
deterministic checks.

## Safety Rules

- Never read, print, copy into chat, or include in a command result the contents
  of `.env`, API keys, passwords, firmware secret headers, databases, backups,
  or telemetry rows.
- Never request a secret through chat or a question tool. When an interactive
  check requires one, instruct the user to type it directly into the terminal
  prompt and pause until that interaction is complete.
- Never deploy a dirty working tree, an abbreviated SHA, an uncommitted change,
  or a revision whose required GitHub checks have not succeeded.
- Never package the working directory or use `git archive HEAD:server`. Build
  the archive with `server/tools/New-ServerReleaseArchive.ps1` from the
  repository root.
- Confirm the SSH target and the exact remote project directory before the
  first remote mutation. Do not infer a production target from shell history.
- Stop before source replacement if preflight, CI, archive audit, checksum,
  database backup, source backup, environment backup, or rollback-image tagging
  fails.
- Do not repair release shell modes with remote `chmod`. A mode or CRLF failure
  means the release archive or transfer path is invalid and must be rebuilt.
- Never extract a release over the current source tree. Validate it in an empty
   sibling staging directory, copy `.env` and `backups/` there, then atomically
   rename directories. Retain the previous source through acceptance.
- Never overwrite a live SQLite database or copy a database into a running API
   container. Use the runbook's offline staged restore and integrity checks.
- Do not change production during a review or `dry-run`; report each command
  that would run and the evidence still required.

## Inputs

Obtain or confirm these values before execution:

1. The revision to deploy, resolved to a full 40-character lowercase commit SHA.
2. The SSH target selected by the user.
3. The absolute or home-relative remote project directory selected by the user.
4. The public HTTPS host expected after deployment.
5. Whether this is a dry run or an authorized production change.

Do not accept a branch name as the final release identity. Resolve it once, then
use the immutable SHA for CI checks, archive creation, transfer naming, image
labeling, verification, and the final report.

## Workflow

### 1. Establish Local Evidence

1. Read the deployment runbook and the current deployment, backup, Compose, and
   archive scripts. Use their current behavior rather than remembered commands.
2. Verify the repository root, current branch, clean status, configured remote,
   and resolved commit SHA.
   Never print a raw remote URL. Strip URI userinfo before reporting only the
   expected host and repository because HTTPS remotes can embed credentials.
3. Fetch remote metadata without changing the worktree. Confirm the selected
   SHA exists on the expected GitHub repository and is appropriate for release.
4. Check the required GitHub Actions runs for that exact SHA. Treat missing,
   queued, cancelled, skipped-required, or failed checks as a stop condition.
5. Run the focused local server release tests and shell syntax check. Do not
   bypass a failure because production currently appears healthy.

### 2. Build the Release Artifact

From the repository root, invoke:

```powershell
./server/tools/New-ServerReleaseArchive.ps1 `
    -Revision <full-sha> `
    -OutputPath <unique-temporary-tar-gz-path>
```

Record the returned revision, archive path, Git file count, tar member count,
shell-script count, byte size, and SHA-256. The helper must complete
successfully. Do not open or augment the archive after its audit.

### 3. Confirm the Remote Boundary

Use read-only SSH commands first. Confirm:

- the remote identity and hostname match the user's target;
- the project directory is exactly the confirmed path;
- Docker Engine and the Compose plugin are available;
- `docker compose up --help` includes both `--wait` and `--wait-timeout`;
- the current Compose project and API container are identifiable;
- free disk space is sufficient for one build plus retained backups;
- `.env` exists with mode `600`, without displaying its contents;
- each required environment key occurs exactly once and is nonempty, without
  displaying any value.

If a release introduces a missing public configuration key, state the key and
the proposed non-secret value, obtain confirmation, update it without echoing
the file, reapply mode `600`, and rerun the count/nonempty checks.

### 4. Create Rollback Assets

Follow the Production Upgrade section of the runbook before transferring or
extracting new source. Create and verify all five assets:

1. A consistent SQLite Online Backup API backup.
2. A source archive that excludes `.env` and the backup directory.
3. A mode-`600` copy of `.env` stored in the protected backup directory.
4. A timestamped rollback tag for the exact image used by the running API
   container.
5. A protected metadata file containing that container's exact image ID and
   configured image reference.

Record the shared UTC timestamp, paths, backup sizes, database backup SHA-256,
old image ID and reference, and rollback image tag. Do not query telemetry
data. A backup command printing a path is acceptable; a command printing
configuration values or database rows is not.

### 5. Transfer and Deploy

1. Transfer the audited archive to a unique remote filename containing the SHA.
2. Compute SHA-256 remotely and require an exact match with the local checksum.
3. Extract from the repository-root archive with `--strip-components=1` into a
   new empty staging directory.
4. Verify required files plus tracked shell executable modes and LF shebangs in
   staging. Rebuild the artifact instead of mutating modes or line endings
   remotely if any check fails.
5. Guard the exact project path, copy `.env` and `backups/` into staging, then
   use the runbook's sibling-directory rename and failure trap. Record and
   retain the previous source path through post-deployment acceptance.
6. Deploy with the exact revision:

```bash
RELEASE_REVISION=<full-sha> ./scripts/deploy.sh
```

Do not report success merely because `docker compose up` returned. The script's
Compose health wait and every acceptance check below must pass.

### 6. Run Acceptance Checks

Verify all of the following without exposing credentials or telemetry rows:

1. Compose reports the API healthy and Caddy running.
2. The API container image label
   `org.opencontainers.image.revision` exactly matches the selected SHA.
3. Internal and external `/healthz` requests return HTTP 200 over the expected
   routes, including public TLS.
4. Dashboard/query requests without credentials and telemetry writes with a
   known-invalid synthetic key return HTTP 401.
5. An authenticated telemetry write and read succeed. Have the user enter any
   required secret directly into an interactive terminal prompt; never relay it
   through the model.
6. SQLite `PRAGMA quick_check` returns only `ok`, WAL mode remains enabled, and
   no telemetry rows are printed.
7. Caddy logs do not contain the known-invalid synthetic key used above,
   proving request-header redaction still applies.
8. A new post-deployment database backup succeeds and its SHA-256 is recorded.

### 7. Handle Failure

If a pre-mutation gate fails, stop without changing production. If deployment
or acceptance fails after mutation:

1. Capture bounded Compose status and logs with secrets redacted.
2. Preserve the failed release revision and image identity for diagnosis.
3. Confirm the pre-upgrade timestamp and all rollback assets still exist.
4. Follow Application Rollback in the runbook using the matching source,
   environment copy, image metadata, rollback tag, and Compose override.
5. Re-run health, authentication-negative, image-identity, SQLite-integrity,
   and backup checks after rollback.

Do not restore the database unless the failed release changed or damaged data.
An application rollback normally keeps the persistent SQLite volume. If a
database restore is required after rollback, pass the retained Compose override
as documented so one-off containers use the exact saved image.

## Final Report

Report the immutable revision, CI result, archive SHA-256, remote target and
project path, pre/post database backup paths and checksums, source/environment
backup paths, old and new image IDs, rollback tag, runtime versions, health and
authentication outcomes, SQLite integrity result, and whether rollback was
needed. Never include secrets, environment values, telemetry rows, or complete
logs.