# Security Policy

## Reporting

Use GitHub private vulnerability reporting when it is enabled for this
repository. Do not open a public issue containing credentials, device IDs,
network addresses, database contents, or exploit details for an active
deployment.

## Credential Exposure

If a device key or dashboard password is exposed:

1. Remove the value from logs, artifacts, and Git history where possible.
2. Fix the logging or packaging path that exposed it.
3. Generate a new independent credential.
4. Update the server environment and restart the deployment.
5. Update and reflash affected firmware when the device key changes.
6. Verify that old credentials receive `401` and inspect access logs for abuse.

Caddy access logs must retain the
`request>headers>X-Device-Key delete` filter. Treat logs created before that
filter was deployed as potentially containing device credentials.

## Deployment Scope

The project is designed for a small trusted deployment. It currently has one
shared writer key, HTTP Basic for operator access, no per-device credential
binding, and an unauthenticated local-LAN endpoint. Operators are responsible
for rate limiting, monitoring, backups, credential rotation, and keeping the
host and container images patched.