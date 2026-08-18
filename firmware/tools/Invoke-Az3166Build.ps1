[CmdletBinding()]
param(
    [ValidateSet("Verify", "Upload")]
    [string]$Action = "Verify",

    [Parameter(Mandatory = $true)]
    [string]$Sketch,

    [string]$Port,

    [string]$Board = "AZ3166:stm32f4:MXCHIP_AZ3166",

    [string]$ArduinoExecutable,

    [string]$ArduinoInstallRoot = (Join-Path (Get-Location) ".tools")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Find-ArduinoExecutable {
    param([string]$RequestedExecutable)

    $candidates = [System.Collections.Generic.List[string]]::new()
    if ($RequestedExecutable) {
        $candidates.Add($RequestedExecutable)
    }
    if ($env:ARDUINO_IDE_PATH) {
        $candidates.Add($env:ARDUINO_IDE_PATH)
    }
    $candidates.Add((Join-Path $ArduinoInstallRoot "arduino-1.8.19\arduino_debug.exe"))
    if (${env:ProgramFiles(x86)}) {
        $candidates.Add((Join-Path ${env:ProgramFiles(x86)} "Arduino\arduino_debug.exe"))
    }
    if ($env:ProgramFiles) {
        $candidates.Add((Join-Path $env:ProgramFiles "Arduino\arduino_debug.exe"))
    }

    $command = Get-Command "arduino_debug.exe" -ErrorAction SilentlyContinue
    if ($command) {
        $candidates.Add($command.Source)
    }

    $executable = $candidates |
        Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } |
        Select-Object -First 1
    if (-not $executable) {
        throw "Arduino IDE 1.8.19 was not found. Pass -ArduinoExecutable or set ARDUINO_IDE_PATH."
    }

    return $executable
}

$resolvedSketch = (Resolve-Path -LiteralPath $Sketch).Path
if ([System.IO.Path]::GetExtension($resolvedSketch) -ne ".ino") {
    throw "Sketch must be an .ino file: $resolvedSketch"
}

$arduino = Find-ArduinoExecutable -RequestedExecutable $ArduinoExecutable
if ($Action -eq "Upload") {
    if ($Port -notmatch "^COM\d+$") {
        throw "Upload requires an explicit ST-Link port such as -Port COM3."
    }
    $arguments = @("--upload", "--board", $Board, "--port", $Port, $resolvedSketch)
    Write-Host "Uploading $resolvedSketch to $Board on $Port"
} else {
    $arguments = @("--verify", "--board", $Board, $resolvedSketch)
    Write-Host "Verifying $resolvedSketch for $Board"
}

$output = (& $arduino @arguments 2>&1 | Out-String)
$exitCode = $LASTEXITCODE
Write-Host $output

if ($exitCode -ne 0) {
    throw "Arduino $Action failed with exit code $exitCode."
}
if ($Action -eq "Upload" -and $output -notmatch "\*\*\s+Verified OK\s+\*\*") {
    throw "Upload exited successfully, but OpenOCD did not report Verified OK."
}

Write-Host "AZ3166 $Action completed successfully."