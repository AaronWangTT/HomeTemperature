[CmdletBinding()]
param(
    [ValidateSet("Verify", "Upload", "Restore")]
    [string]$Action = "Verify",

    [Parameter(Mandatory = $true)]
    [string]$Sketch,

    [string]$Port,

    [string]$Board = "AZ3166:stm32f4:MXCHIP_AZ3166",

    [string]$ArduinoExecutable,

    [string]$ArduinoInstallRoot = (Join-Path (Get-Location) ".tools"),

    [string]$BuildPath,

    [string]$OtaBuildConfig,

    [string]$StLinkSerial,

    [string]$OtaPublicKey,

    [string]$FirmwareVersion,

    [string]$SourceCommit,

    [string]$PythonExecutable = "python"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$coreVersion = "3.1.2"
$coreArchiveSha256 = "3f45783bb736c9934dc5993076eb619e4877333f05a1c2f81feffe76e97f644f"
$libraryVersion = "1.1.1"
$libraryArchiveSha256 = "7c65dc64220ed25be7e55a8e11ac138210c4502977fc72808730251e373ddd80"
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

$productionSketch = (
    Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\AZ3166\AZ3166.ino")
).Path
$isProductionSketch = $resolvedSketch -eq $productionSketch
$temporaryBuildPath = $null
$temporaryRecipePath = $null
$resolvedBuildPath = $null

trap {
    $failure = $_
    if ($temporaryBuildPath -and (Test-Path -LiteralPath $temporaryBuildPath)) {
        Remove-Item -LiteralPath $temporaryBuildPath -Recurse -Force
    }
    if ($temporaryRecipePath -and (Test-Path -LiteralPath $temporaryRecipePath)) {
        Remove-Item -LiteralPath $temporaryRecipePath -Recurse -Force
    }
    throw $failure
}

if ($OtaBuildConfig -and -not $isProductionSketch) {
    throw "-OtaBuildConfig is valid only for the production AZ3166 sketch."
}
if (
    ($Action -eq "Upload" -or $Action -eq "Restore") -and
    $isProductionSketch -and
    $OtaBuildConfig
) {
    throw "Production Upload and Restore do not accept -OtaBuildConfig."
}
if ($Action -eq "Restore" -and -not $isProductionSketch) {
    throw "Restore is valid only for the production AZ3166 sketch."
}
if ($isProductionSketch -and -not $BuildPath) {
    $temporaryBuildPath = Join-Path (
        [System.IO.Path]::GetTempPath()
    ) ("hometemperature-ota-build-" + [Guid]::NewGuid().ToString("N"))
    $BuildPath = $temporaryBuildPath
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

if ($Action -eq "Upload" -and -not $isProductionSketch) {
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
    if (($Action -eq "Upload" -or $Action -eq "Restore") -and $Port) {
        throw "Production Upload and Restore select the probe by -StLinkSerial, not -Port."
    }
    if (
        ($Action -eq "Upload" -or $Action -eq "Restore") -and
        $StLinkSerial -notmatch "^[0-9A-Fa-f]{24}$"
    ) {
        throw "Production Upload and Restore require the exact 24-hex-character -StLinkSerial."
    }
    $arguments = @(
        "--verify", "--board", $Board,
        "--pref", "sketchbook.path=$arduinoSketchbook",
        $resolvedSketch
    )
    Write-Host "Verifying $resolvedSketch for $Board"
}

if ($isProductionSketch) {
    $sourceLinkerScript = (
        Resolve-Path -LiteralPath (
            Join-Path $PSScriptRoot "..\AZ3166\linker\AZ3166-ota.ld"
        )
    ).Path
    if (-not $env:PUBLIC -or $env:PUBLIC -match "\s") {
        throw "The user-writable PUBLIC path must exist and contain no whitespace."
    }
    $temporaryRecipePath = Join-Path (
        Join-Path $env:PUBLIC "HomeTemperatureOtaBuild"
    ) ([Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $temporaryRecipePath | Out-Null
    $linkerScript = Join-Path $temporaryRecipePath "AZ3166-ota.ld"
    Copy-Item -LiteralPath $sourceLinkerScript -Destination $linkerScript
    $arguments += @(
        "--pref",
        "compiler.link.script.flags=-T$linkerScript"
    )
}
if ($BuildPath) {
    $resolvedBuildPath = [System.IO.Path]::GetFullPath($BuildPath)
    New-Item -ItemType Directory -Force -Path $resolvedBuildPath | Out-Null
    $arguments += @("--pref", "build.path=$resolvedBuildPath")
}
if ($Action -eq "Upload" -and $isProductionSketch) {
    if (
        -not $OtaPublicKey -or
        -not $FirmwareVersion -or
        -not $SourceCommit
    ) {
        throw "Production Upload requires -OtaPublicKey, -FirmwareVersion, and -SourceCommit."
    }
    $resolvedOtaPublicKey = (Resolve-Path -LiteralPath $OtaPublicKey).Path
    $otaCli = (
        Resolve-Path -LiteralPath (
            Join-Path $PSScriptRoot "..\..\tools\ota\ota_cli.py"
        )
    ).Path
    $generatedOtaBuildConfig = Join-Path $resolvedBuildPath "ota-build-config.generated.h"
    $configOutput = (& $PythonExecutable $otaCli build-config `
        --public-key $resolvedOtaPublicKey `
        --output $generatedOtaBuildConfig `
        --version $FirmwareVersion `
        --source $SourceCommit 2>&1 | Out-String)
    $configExitCode = $LASTEXITCODE
    Write-Host $configOutput
    if ($configExitCode -ne 0) {
        if ($temporaryBuildPath) {
            Remove-Item -LiteralPath $temporaryBuildPath -Recurse -Force
        }
        throw "OTA build configuration failed with exit code $configExitCode."
    }
    $OtaBuildConfig = $generatedOtaBuildConfig
}
if ($OtaBuildConfig) {
    $resolvedOtaBuildConfig = (Resolve-Path -LiteralPath $OtaBuildConfig).Path
    $stagedOtaBuildConfig = Join-Path $temporaryRecipePath "ota-build-config.h"
    Copy-Item -LiteralPath $resolvedOtaBuildConfig -Destination $stagedOtaBuildConfig
    $arguments += @(
        "--pref",
        "compiler.cpp.extra_flags=-include $stagedOtaBuildConfig"
    )
}

$output = (& $arduino @arguments 2>&1 | Out-String)
$exitCode = $LASTEXITCODE
Write-Host $output

if ($exitCode -ne 0) {
    if ($temporaryBuildPath) {
        Remove-Item -LiteralPath $temporaryBuildPath -Recurse -Force
    }
    if ($temporaryRecipePath) {
        Remove-Item -LiteralPath $temporaryRecipePath -Recurse -Force
    }
    throw "Arduino $Action failed with exit code $exitCode."
}
if (
    $Action -eq "Upload" -and
    -not $isProductionSketch -and
    $output -notmatch "\*\*\s+Verified OK\s+\*\*"
) {
    if ($temporaryBuildPath) {
        Remove-Item -LiteralPath $temporaryBuildPath -Recurse -Force
    }
    throw "Upload exited successfully, but OpenOCD did not report Verified OK."
}
if ($isProductionSketch) {
    $binaryPath = Join-Path $resolvedBuildPath "AZ3166.ino.bin"
    if (-not (Test-Path -LiteralPath $binaryPath -PathType Leaf)) {
        if ($temporaryBuildPath) {
            Remove-Item -LiteralPath $temporaryBuildPath -Recurse -Force
        }
        throw "Production build did not emit $binaryPath."
    }
    $binary = [System.IO.File]::ReadAllBytes($binaryPath)
    $descriptorMagic = if ($binary.Length -ge 0x300) {
        [System.Text.Encoding]::ASCII.GetString($binary, 0x200, 8)
    } else {
        ""
    }
    $descriptorSize = if ($binary.Length -ge 0x20C) {
        [uint16](
            [uint16]$binary[0x20A] -bor
            ([uint16]$binary[0x20B] -shl 8)
        )
    } else {
        0
    }
    if ($descriptorMagic -ne "AZOTA001" -or $descriptorSize -ne 256) {
        if ($temporaryBuildPath) {
            Remove-Item -LiteralPath $temporaryBuildPath -Recurse -Force
        }
        throw "Production binary has no valid OTA descriptor at offset 0x200."
    }
    if ($Action -eq "Upload") {
        $validationOutput = (& $PythonExecutable $otaCli validate-image `
            --image $binaryPath `
            --public-key $resolvedOtaPublicKey `
            --version $FirmwareVersion `
            --source $SourceCommit 2>&1 | Out-String)
        $validationExitCode = $LASTEXITCODE
        Write-Host $validationOutput
        if ($validationExitCode -ne 0) {
            if ($temporaryBuildPath) {
                Remove-Item -LiteralPath $temporaryBuildPath -Recurse -Force
            }
            throw "Production image validation failed with exit code $validationExitCode."
        }
    }
    if ($Action -eq "Upload" -or $Action -eq "Restore") {
        $openOcdRoot = Split-Path -Parent (Split-Path -Parent $installedOpenOcd)
        $interfaceConfig = Join-Path $openOcdRoot "scripts\interface\stlink-v2-1.cfg"
        $targetConfig = Join-Path $openOcdRoot "scripts\target\stm32f4x.cfg"
        $probeSelection = @("-c", "hla_serial $StLinkSerial")
        if ($Action -eq "Upload") {
            Write-Host "Uploading validated $binaryPath to $Board through ST-Link $StLinkSerial"
        } else {
            Write-Host "Restoring validated fail-closed production firmware through ST-Link $StLinkSerial"
        }
        $uploadOutput = (& $installedOpenOcd `
            "-f" $interfaceConfig `
            "-c" "transport select hla_swd" `
            @probeSelection `
            "-f" $targetConfig `
            "-c" "program {$binaryPath} verify reset 0x800C000; shutdown" `
            2>&1 | Out-String)
        $uploadExitCode = $LASTEXITCODE
        Write-Host $uploadOutput
        if ($uploadExitCode -ne 0) {
            if ($temporaryBuildPath) {
                Remove-Item -LiteralPath $temporaryBuildPath -Recurse -Force
            }
            throw "OpenOCD upload failed with exit code $uploadExitCode."
        }
        if ($uploadOutput -notmatch "\*\*\s+Verified OK\s+\*\*") {
            if ($temporaryBuildPath) {
                Remove-Item -LiteralPath $temporaryBuildPath -Recurse -Force
            }
            throw "OpenOCD exited successfully, but did not report Verified OK."
        }
    }
}
if ($temporaryBuildPath) {
    Remove-Item -LiteralPath $temporaryBuildPath -Recurse -Force
}
if ($temporaryRecipePath) {
    Remove-Item -LiteralPath $temporaryRecipePath -Recurse -Force
}

Write-Host "AZ3166 $Action completed successfully."