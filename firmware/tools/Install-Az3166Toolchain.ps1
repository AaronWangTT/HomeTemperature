[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ArduinoExecutable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$boardManagerUrl = "https://raw.githubusercontent.com/VSChina/azureiotdevkit_tools/d0c76e57d1ad62610aab0773ba687d55df2e4c91/package_azureboard_index.json"
$core = "AZ3166:stm32f4:2.0.0"
$resolvedArduino = (Resolve-Path -LiteralPath $ArduinoExecutable).Path

$versionOutput = (& $resolvedArduino --version 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $versionOutput -notmatch "1\.8\.19") {
    throw "Arduino IDE 1.8.19 is required. Detected: $($versionOutput.Trim())"
}

Write-Host "Installing $core from the pinned board package index..."
& $resolvedArduino `
    --install-boards $core `
    --pref "boardsmanager.additional.urls=$boardManagerUrl" `
    --save-prefs
if ($LASTEXITCODE -ne 0) {
    throw "Failed to install $core."
}

Write-Host "AZ3166 Core 2.0.0 installation completed."