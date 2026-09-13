[CmdletBinding()]
param(
    [string]$ArduinoExecutable,

    [string]$ArduinoInstallRoot = (Join-Path (Get-Location) ".tools")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$boardManagerUrl = "https://raw.githubusercontent.com/AaronWangTT/azureiotdevkit_tools/ac5055ec3a8fddd3135b1c367a9f630d33f65ef8/package_azureboard_index.json"
$coreVersion = "2.0.2"
$core = "AZ3166:stm32f4:$coreVersion"
$coreArchiveSha256 = "5914d3e7b988fdc50b00ff241b7ac191fd6fb9bdc234b4a3f86c51ed0d5e9677"
$compilerVersion = "5_4-2016q3"
$openOcdVersion = "0.10.0"
$arduinoVersion = "1.8.19"
$arduinoArchiveUrl = "https://downloads.arduino.cc/arduino-$arduinoVersion-windows.zip"
$arduinoInstallDir = Join-Path $ArduinoInstallRoot "arduino-$arduinoVersion"
$arduinoArchive = Join-Path $ArduinoInstallRoot "arduino-$arduinoVersion-windows.zip"
$bundledArduinoExecutable = Join-Path $arduinoInstallDir "arduino_debug.exe"
$arduinoDataRoot = Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "Arduino15"
$installedCoreRoot = Join-Path $arduinoDataRoot "packages\AZ3166\hardware\stm32f4\$coreVersion"
$coreStamp = Join-Path $installedCoreRoot ".hometemperature-source.sha256"
$installedCompilerRoot = Join-Path $arduinoDataRoot "packages\AZ3166\tools\arm-none-eabi-gcc\$compilerVersion"
$installedCompiler = Join-Path $installedCompilerRoot "bin\arm-none-eabi-g++.exe"
$installedOpenOcdRoot = Join-Path $arduinoDataRoot "packages\AZ3166\tools\openocd\$openOcdVersion"
$installedOpenOcd = Join-Path $installedOpenOcdRoot "bin\openocd.exe"
$arduinoSketchbook = Join-Path $ArduinoInstallRoot "sketchbook"
$libraryVersion = "1.1.0"
$libraryArchiveUrl = "https://github.com/AaronWangTT/ArduinoMDNS/releases/download/1.1.0/ArduinoMDNS-1.1.0.zip"
$libraryArchiveSha256 = "f7a4c6f53d614d05aef3c6c02f6f49b4057202a42a8e40f63bdb062e99162e47"
$installedLibraryRoot = Join-Path $arduinoSketchbook "libraries\ArduinoMDNS"
$libraryStamp = Join-Path $installedLibraryRoot ".hometemperature-source.sha256"

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

function Test-Az3166CoreInstallation {
    if (
        -not (Test-Path -LiteralPath (Join-Path $installedCoreRoot "platform.txt") -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $installedCoreRoot "boards.txt") -PathType Leaf) -or
        -not (Test-Path -LiteralPath $installedCompiler -PathType Leaf) -or
        -not (Test-Path -LiteralPath $installedOpenOcd -PathType Leaf) -or
        -not (Test-Path -LiteralPath $coreStamp -PathType Leaf)
    ) {
        return $false
    }

    $installedHash = (Get-Content -Raw -LiteralPath $coreStamp).Trim()
    return $installedHash -eq $coreArchiveSha256
}

function Test-ArduinoMdnsInstallation {
    $properties = Join-Path $installedLibraryRoot "library.properties"
    $transportHeader = Join-Path $installedLibraryRoot "MDNSTransport.h"
    if (
        -not (Test-Path -LiteralPath $properties -PathType Leaf) -or
        -not (Test-Path -LiteralPath $transportHeader -PathType Leaf) -or
        -not (Test-Path -LiteralPath $libraryStamp -PathType Leaf)
    ) {
        return $false
    }
    if ((Get-Content -Raw -LiteralPath $properties) -notmatch "(?m)^version=$([regex]::Escape($libraryVersion))\s*$") {
        return $false
    }

    $installedHash = (Get-Content -Raw -LiteralPath $libraryStamp).Trim()
    return $installedHash -eq $libraryArchiveSha256
}

function Install-ArduinoMdns {
    New-Item -ItemType Directory -Path $ArduinoInstallRoot -Force | Out-Null
    $archive = Join-Path $ArduinoInstallRoot "ArduinoMDNS-$libraryVersion.zip"
    $extractRoot = Join-Path $ArduinoInstallRoot "ArduinoMDNS-$libraryVersion-extract"

    Write-Host "Downloading ArduinoMDNS $libraryVersion..."
    try {
        Invoke-WebRequest -Uri $libraryArchiveUrl -OutFile $archive
        $actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $libraryArchiveSha256) {
            throw "ArduinoMDNS checksum mismatch. Expected $libraryArchiveSha256, received $actualHash."
        }

        Remove-Item -LiteralPath $extractRoot -Recurse -Force -ErrorAction SilentlyContinue
        Expand-Archive -LiteralPath $archive -DestinationPath $extractRoot
        $extractedLibrary = Join-Path $extractRoot "ArduinoMDNS"
        $properties = Join-Path $extractedLibrary "library.properties"
        $transportHeader = Join-Path $extractedLibrary "MDNSTransport.h"
        if (-not (Test-Path -LiteralPath $properties -PathType Leaf)) {
            throw "ArduinoMDNS archive does not contain the expected library root."
        }
        if ((Get-Content -Raw -LiteralPath $properties) -notmatch "(?m)^version=$([regex]::Escape($libraryVersion))\s*$") {
            throw "ArduinoMDNS archive does not declare version $libraryVersion."
        }
        if (-not (Test-Path -LiteralPath $transportHeader -PathType Leaf)) {
            throw "ArduinoMDNS archive does not contain the required MDNSTransport.h header."
        }

        New-Item -ItemType Directory -Path (Split-Path -Parent $installedLibraryRoot) -Force | Out-Null
        Remove-Item -LiteralPath $installedLibraryRoot -Recurse -Force -ErrorAction SilentlyContinue
        Move-Item -LiteralPath $extractedLibrary -Destination $installedLibraryRoot
        Set-Content -LiteralPath $libraryStamp -Value $libraryArchiveSha256 -NoNewline
    } finally {
        Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $extractRoot -Recurse -Force -ErrorAction SilentlyContinue
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

if (Test-Az3166CoreInstallation) {
    Write-Host "$core is already installed."
} else {
    if (Test-Path -LiteralPath $installedCoreRoot -PathType Container) {
        Write-Host "Removing unverified $core installation before repair..."
        Remove-Item -LiteralPath $installedCoreRoot -Recurse -Force
    }

    Write-Host "Installing $core from the pinned board package index..."
    & $resolvedArduino `
        --install-boards $core `
        --pref "boardsmanager.additional.urls=$boardManagerUrl" `
        --save-prefs
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install $core."
    }
    if (
        -not (Test-Path -LiteralPath (Join-Path $installedCoreRoot "platform.txt") -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $installedCoreRoot "boards.txt") -PathType Leaf) -or
        -not (Test-Path -LiteralPath $installedCompiler -PathType Leaf) -or
        -not (Test-Path -LiteralPath $installedOpenOcd -PathType Leaf)
    ) {
        throw "$core installation completed without all required board and tool files."
    }
    Set-Content -LiteralPath $coreStamp -Value $coreArchiveSha256 -NoNewline
}

if (Test-ArduinoMdnsInstallation) {
    Write-Host "ArduinoMDNS $libraryVersion is already installed."
} else {
    Install-ArduinoMdns
}

Write-Host "AZ3166 Core $coreVersion and ArduinoMDNS $libraryVersion installation completed."