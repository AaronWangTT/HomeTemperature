param(
    [ValidateSet("Verify", "Run")]
    [string]$Action = "Verify",

    [string]$Port
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($Action -eq "Run" -and [string]::IsNullOrWhiteSpace($Port)) {
    throw "Run requires an explicit ST-Link port, for example: -Port COM3"
}

$firmwareRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $firmwareRoot "tools\Invoke-Az3166Build.ps1"
$productionSketch = Join-Path $firmwareRoot "AZ3166\AZ3166.ino"
$runners = @(
    "run-button-debouncer-tests.ps1",
    "run-button-controller-tests.ps1",
    "run-device-identity-tests.ps1",
    "run-telemetry-service-tests.ps1",
    "run-local-web-server-tests.ps1",
    "run-upload-scheduler-tests.ps1",
    "run-cloud-telemetry-tests.ps1",
    "run-cloud-upload-controller-tests.ps1",
    "run-connectivity-manager-tests.ps1",
    "run-telemetry-uploader-tests.ps1"
)

& $buildScript -Action Verify -Sketch $productionSketch

foreach ($runner in $runners) {
    $arguments = @{
        Action = $Action
    }
    if ($Action -eq "Run") {
        $arguments.Port = $Port
    }

    Write-Host "`n=== $runner ($Action) ==="
    & (Join-Path $PSScriptRoot $runner) @arguments
}

Write-Host "`nAll AZ3166 suites completed successfully."