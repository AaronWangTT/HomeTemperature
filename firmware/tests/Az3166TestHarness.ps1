function Invoke-Az3166TestSuite {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SketchRoot,

        [Parameter(Mandatory = $true)]
        [string]$SourceRoot,

        [Parameter(Mandatory = $true)]
        [string]$SuiteName,

        [Parameter(Mandatory = $true)]
        [string]$TestSource,

        [Parameter(Mandatory = $true)]
        [string[]]$SourceFiles,

        [switch]$StageSourcesUnderSrc,

        [ValidateSet("Verify", "Run")]
        [string]$Action = "Verify",

        [string]$Port
    )

    $ErrorActionPreference = "Stop"

    $productionSketch = Join-Path $SketchRoot "AZ3166.ino"
    $firmwareRoot = Split-Path -Parent $PSScriptRoot
    $buildUploadScript = Join-Path $firmwareRoot "tools\Invoke-Az3166Build.ps1"
    $repositoryRoot = Split-Path -Parent $firmwareRoot
    $stagingRoot = Join-Path $repositoryRoot ".test-staging\az3166-$($SuiteName.ToLowerInvariant())"
    $stagingSketch = Join-Path $stagingRoot $SuiteName

    if (-not (Test-Path $buildUploadScript)) {
        throw "Repository AZ3166 build helper not found: $buildUploadScript"
    }
    if ($Action -eq "Run" -and [string]::IsNullOrWhiteSpace($Port)) {
        throw "Run requires an explicit ST-Link port, for example: -Port COM3"
    }
    if (
        $Action -eq "Run" -and
        $env:HOME_TEMPERATURE_STLINK_SERIAL -notmatch "^[0-9A-Fa-f]{24}$"
    ) {
        throw "Run requires HOME_TEMPERATURE_STLINK_SERIAL as exactly 24 hexadecimal characters."
    }
    if (
        $Action -eq "Run" -and
        $env:HOME_TEMPERATURE_STLINK_SERIAL -notmatch "^[0-9A-Fa-f]{24}$"
    ) {
        throw "Run requires HOME_TEMPERATURE_STLINK_SERIAL as exactly 24 hexadecimal characters."
    }

    $invokeBuild = {
        param(
            [string]$BuildAction,
            [string]$Sketch
        )

        $arguments = @{
            Action = $BuildAction
            Sketch = $Sketch
        }
        $restoringProduction = (
            $BuildAction -eq "Restore" -and
            $Sketch -eq $productionSketch
        )
        if ($restoringProduction) {
            $arguments.StLinkSerial = $env:HOME_TEMPERATURE_STLINK_SERIAL
        } elseif (-not [string]::IsNullOrWhiteSpace($Port)) {
            $arguments.Port = $Port
        }

        & $buildUploadScript @arguments
        if ($LASTEXITCODE -ne 0) {
            throw "$BuildAction failed for $Sketch"
        }
    }

    $runTests = {
        $serial = New-Object System.IO.Ports.SerialPort
        $serial.PortName = $Port
        $serial.BaudRate = 115200
        $serial.DataBits = 8
        $serial.Parity = [System.IO.Ports.Parity]::None
        $serial.StopBits = [System.IO.Ports.StopBits]::One
        $serial.ReadTimeout = 1000
        $serial.DtrEnable = $true

        try {
            $serial.Open()
            $serial.DiscardInBuffer()
            $deadline = [DateTime]::UtcNow.AddSeconds(20)
            $suiteMarker = "TEST_SUITE: $SuiteName"
            $suiteStarted = $false
            while ([DateTime]::UtcNow -lt $deadline) {
                try {
                    $line = $serial.ReadLine().Trim()
                    Write-Host $line
                    if ($line -eq $suiteMarker) {
                        $suiteStarted = $true
                        continue
                    }
                    if ($suiteStarted -and $line -eq "TEST_RESULT: PASS") {
                        return
                    }
                    if ($suiteStarted -and $line -eq "TEST_RESULT: FAIL") {
                        throw "$SuiteName failed"
                    }
                } catch [System.TimeoutException] {
                }
            }

            if (-not $suiteStarted) {
                throw "Timed out waiting for $suiteMarker on $Port"
            }
            throw "Timed out waiting for the $SuiteName result on $Port"
        } finally {
            if ($serial.IsOpen) {
                $serial.Close()
            }
            $serial.Dispose()
        }
    }

    Remove-Item $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
    New-Item $stagingSketch -ItemType Directory -Force | Out-Null

    try {
        Copy-Item $TestSource $stagingSketch
        $stagingSource = $stagingSketch
        if ($StageSourcesUnderSrc) {
            $stagingSource = Join-Path $stagingSketch "src"
            New-Item $stagingSource -ItemType Directory -Force | Out-Null
        }
        foreach ($sourceFile in $SourceFiles) {
            $destination = Join-Path $stagingSource $sourceFile
            New-Item (Split-Path -Parent $destination) -ItemType Directory -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $SourceRoot $sourceFile) `
                -Destination $destination -Recurse
        }

        $testSketch = Join-Path $stagingSketch "$SuiteName.ino"
        if ($Action -eq "Verify") {
            & $invokeBuild "Verify" $testSketch
        } else {
            $testError = $null
            try {
                & $invokeBuild "Upload" $testSketch
                & $runTests
            } catch {
                $testError = $_
            } finally {
                Write-Host "Restoring production firmware..."
                & $invokeBuild "Restore" $productionSketch
            }

            if ($null -ne $testError) {
                throw $testError
            }
        }
    } finally {
        Remove-Item $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}