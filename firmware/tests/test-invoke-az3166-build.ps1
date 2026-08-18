Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

$firmwareRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $firmwareRoot "tools\Invoke-Az3166Build.ps1"
$sketch = Join-Path $firmwareRoot "AZ3166\AZ3166.ino"
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("az3166-build-test-" + [guid]::NewGuid())
$mockArduino = Join-Path $tempRoot "mock-arduino.ps1"
$artifactDirectory = Join-Path $tempRoot "artifacts"
$argumentLog = Join-Path $tempRoot "arguments.txt"

New-Item -ItemType Directory -Path $tempRoot | Out-Null
@'
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)

$Arguments | Set-Content -LiteralPath $env:AZ3166_BUILD_ARGUMENT_LOG
$buildPreference = $Arguments | Where-Object { $_ -like "build.path=*" } | Select-Object -First 1
$buildPath = $buildPreference.Substring("build.path=".Length)
New-Item -ItemType Directory -Path $buildPath -Force | Out-Null
New-Item -ItemType File -Path (Join-Path $buildPath "AZ3166.ino.bin") -Force | Out-Null
New-Item -ItemType File -Path (Join-Path $buildPath "AZ3166.ino.elf") -Force | Out-Null
if ($Arguments -contains "--upload") {
    Write-Output "*** Verified OK **"
}
'@ | Set-Content -LiteralPath $mockArduino

try {
    $env:AZ3166_BUILD_ARGUMENT_LOG = $argumentLog
    $global:LASTEXITCODE = 0
    $output = & $buildScript -Action Verify -Sketch $sketch -ArduinoExecutable $mockArduino -BuildPath $artifactDirectory 6>&1
    $resolvedArtifactDirectory = [System.IO.Path]::GetFullPath($artifactDirectory)
    $arguments = Get-Content -LiteralPath $argumentLog

    Assert-True (Test-Path -LiteralPath (Join-Path $resolvedArtifactDirectory "AZ3166.ino.bin") -PathType Leaf) "Verify did not preserve the binary."
    Assert-True (Test-Path -LiteralPath (Join-Path $resolvedArtifactDirectory "AZ3166.ino.elf") -PathType Leaf) "Verify did not preserve the ELF."
    Assert-True ($arguments -contains "--pref") "Arduino invocation did not include --pref."
    Assert-True ($arguments -contains "build.path=$resolvedArtifactDirectory") "Arduino invocation did not include the normalized build path."
    Assert-True ($arguments -contains "--verify") "Verify invocation was not preserved."
    Assert-True (($output | Out-String) -match [regex]::Escape("Build artifacts: $resolvedArtifactDirectory")) "Artifact directory was not reported."

    $null = & $buildScript -Action Upload -Sketch $sketch -ArduinoExecutable $mockArduino -BuildPath $artifactDirectory -Port COM3
    $arguments = Get-Content -LiteralPath $argumentLog
    Assert-True ($arguments -contains "--upload") "Upload invocation was not preserved."
    Assert-True ($arguments -contains "build.path=$resolvedArtifactDirectory") "Upload invocation did not include the build path."
} finally {
    Remove-Item Env:AZ3166_BUILD_ARGUMENT_LOG -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Invoke-Az3166Build static parameter test passed."
