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

$coreVersion = "2.0.2"
$coreArchiveSha256 = "5914d3e7b988fdc50b00ff241b7ac191fd6fb9bdc234b4a3f86c51ed0d5e9677"
$libraryVersion = "1.1.0"
$libraryArchiveSha256 = "f7a4c6f53d614d05aef3c6c02f6f49b4057202a42a8e40f63bdb062e99162e47"
$compilerVersion = "5_4-2016q3"
$openOcdVersion = "0.10.0"
$arduinoDataRoot = Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "Arduino15"
$installedCoreRoot = Join-Path $arduinoDataRoot "packages\AZ3166\hardware\stm32f4\$coreVersion"
$coreStamp = Join-Path $installedCoreRoot ".hometemperature-source.sha256"
$installedCompiler = Join-Path $arduinoDataRoot "packages\AZ3166\tools\arm-none-eabi-gcc\$compilerVersion\bin\arm-none-eabi-g++.exe"
$installedOpenOcd = Join-Path $arduinoDataRoot "packages\AZ3166\tools\openocd\$openOcdVersion\bin\openocd.exe"
$arduinoSketchbook = Join-Path $ArduinoInstallRoot "sketchbook"
$installedLibraryRoot = Join-Path $arduinoSketchbook "libraries\ArduinoMDNS"
$libraryStamp = Join-Path $installedLibraryRoot ".hometemperature-source.sha256"

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
if (
    -not (Test-Path -LiteralPath (Join-Path $installedCoreRoot "platform.txt") -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $installedCoreRoot "boards.txt") -PathType Leaf) -or
    -not (Test-Path -LiteralPath $installedCompiler -PathType Leaf) -or
    -not (Test-Path -LiteralPath $installedOpenOcd -PathType Leaf) -or
    -not (Test-Path -LiteralPath $coreStamp -PathType Leaf) -or
    (Get-Content -Raw -LiteralPath $coreStamp).Trim() -ne $coreArchiveSha256
) {
    throw "The checksum-pinned AZ3166 Core $coreVersion and its pinned tools are not installed. Run firmware/tools/Install-Az3166Toolchain.ps1."
}

$libraryProperties = Join-Path $installedLibraryRoot "library.properties"
$libraryTransportHeader = Join-Path $installedLibraryRoot "MDNSTransport.h"
if (
    -not (Test-Path -LiteralPath $libraryProperties -PathType Leaf) -or
    (Get-Content -Raw -LiteralPath $libraryProperties) -notmatch "(?m)^version=$([regex]::Escape($libraryVersion))\s*$" -or
    -not (Test-Path -LiteralPath $libraryTransportHeader -PathType Leaf) -or
    -not (Test-Path -LiteralPath $libraryStamp -PathType Leaf) -or
    (Get-Content -Raw -LiteralPath $libraryStamp).Trim() -ne $libraryArchiveSha256
) {
    throw "The checksum-pinned ArduinoMDNS $libraryVersion is not installed in $arduinoSketchbook. Run firmware/tools/Install-Az3166Toolchain.ps1."
}

if ($Action -eq "Upload") {
    if ($Port -notmatch "^COM\d+$") {
        throw "Upload requires an explicit ST-Link port such as -Port COM3."
    }
    $arguments = @(
        "--upload", "--board", $Board,
        "--pref", "sketchbook.path=$arduinoSketchbook",
        "--port", $Port, $resolvedSketch
    )
    Write-Host "Uploading $resolvedSketch to $Board on $Port"
} else {
    $arguments = @(
        "--verify", "--board", $Board,
        "--pref", "sketchbook.path=$arduinoSketchbook",
        $resolvedSketch
    )
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