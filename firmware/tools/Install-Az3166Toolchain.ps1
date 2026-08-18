[CmdletBinding()]
param(
    [string]$ArduinoExecutable,

    [string]$ArduinoInstallRoot = (Join-Path (Get-Location) ".tools")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$boardManagerUrl = "https://raw.githubusercontent.com/VSChina/azureiotdevkit_tools/d0c76e57d1ad62610aab0773ba687d55df2e4c91/package_azureboard_index.json"
$core = "AZ3166:stm32f4:2.0.0"
$arduinoVersion = "1.8.19"
$arduinoArchiveUrl = "https://downloads.arduino.cc/arduino-$arduinoVersion-windows.zip"
$arduinoInstallDir = Join-Path $ArduinoInstallRoot "arduino-$arduinoVersion"
$arduinoArchive = Join-Path $ArduinoInstallRoot "arduino-$arduinoVersion-windows.zip"
$bundledArduinoExecutable = Join-Path $arduinoInstallDir "arduino_debug.exe"
$arduinoDataRoot = Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "Arduino15"
$installedCoreRoot = Join-Path $arduinoDataRoot "packages\AZ3166\hardware\stm32f4\2.0.0"

function Find-ArduinoExecutable {
    $candidates = [System.Collections.Generic.List[string]]::new()
    if ($ArduinoExecutable) {
        $candidates.Add($ArduinoExecutable)
    }
    if ($env:ARDUINO_IDE_PATH) {
        $candidates.Add($env:ARDUINO_IDE_PATH)
    }
    $candidates.Add($bundledArduinoExecutable)
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

    return $candidates |
        Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } |
        Select-Object -First 1
}

function Install-ArduinoIde {
    New-Item -ItemType Directory -Path $ArduinoInstallRoot -Force | Out-Null
    Write-Host "Downloading Arduino IDE $arduinoVersion..."
    Invoke-WebRequest `
        -Uri $arduinoArchiveUrl `
        -OutFile $arduinoArchive
    try {
        Expand-Archive `
            -Path $arduinoArchive `
            -DestinationPath $ArduinoInstallRoot `
            -Force
    } finally {
        Remove-Item -LiteralPath $arduinoArchive -ErrorAction SilentlyContinue
    }

    if (-not (Test-Path -LiteralPath $bundledArduinoExecutable -PathType Leaf)) {
        throw "Arduino IDE $arduinoVersion was downloaded, but arduino_debug.exe was not found at $bundledArduinoExecutable."
    }
}

$resolvedArduino = Find-ArduinoExecutable
if (-not $resolvedArduino) {
    Install-ArduinoIde
    $resolvedArduino = Find-ArduinoExecutable
}

if (-not $resolvedArduino) {
    throw "Arduino IDE $arduinoVersion was not found and automatic installation failed."
}

$resolvedArduino = (Resolve-Path -LiteralPath $resolvedArduino).Path

$versionOutput = (& $resolvedArduino --version 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $versionOutput -notmatch [regex]::Escape($arduinoVersion)) {
    throw "Arduino IDE $arduinoVersion is required. Detected: $($versionOutput.Trim())"
}

if (
    (Test-Path -LiteralPath (Join-Path $installedCoreRoot "platform.txt")) -and
    (Test-Path -LiteralPath (Join-Path $installedCoreRoot "boards.txt"))
) {
    Write-Host "$core is already installed."
} else {
    Write-Host "Installing $core from the pinned board package index..."
    & $resolvedArduino `
        --install-boards $core `
        --pref "boardsmanager.additional.urls=$boardManagerUrl" `
        --save-prefs
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install $core."
    }
}

Write-Host "AZ3166 Core 2.0.0 installation completed."