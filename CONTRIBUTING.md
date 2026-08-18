# Contributing

Keep changes scoped to either the firmware or server ownership boundary and do
not commit device credentials, deployment environment files, databases, logs,
or hardware identifiers.

## Server Checks

```powershell
python -m pip install -r server/requirements-dev.txt
$env:PYTHONPATH = 'server'
python -m pytest server/tests -q
python -m compileall -q server/app server/tests
```

## Firmware Checks

```powershell
& .\firmware\tools\Invoke-Az3166Build.ps1 `
  -Action Verify `
  -Sketch .\firmware\AZ3166\AZ3166.ino
& .\firmware\tests\run-all-tests.ps1 -Action Verify
```

Board uploads require an explicit `-Port COMx`. Confirm OpenOCD `Verified OK`
and restore production firmware before disconnecting the device.

Pull requests should describe behavioral changes, list the checks run, and
state clearly when hardware, Docker, TLS, or deployment verification was not
available.