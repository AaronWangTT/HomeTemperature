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

        [ValidateSet("Verify", "Run")]
        [string]$Action = "Verify",

        [string]$Port
    )

    $ErrorActionPreference = "Stop"

    $productionSketch = Join-Path $SketchRoot "AZ3166.ino"
    $firmwareRoot = Split-Path -Parent $PSScriptRoot
    $buildUploadScript = Join-Path $firmwareRoot "tools\Invoke-Az3166Build.ps1"
    $stagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) "az3166-$($SuiteName.ToLowerInvariant())"
    $stagingSketch = Join-Path $stagingRoot $SuiteName

    if (-not (Test-Path $buildUploadScript)) {
        throw "Repository AZ3166 build helper not found: $buildUploadScript"
    }
    if ($Action -eq "Run" -and [string]::IsNullOrWhiteSpace($Port)) {
        throw "Run requires an explicit ST-Link port, for example: -Port COM3"
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
        if (-not [string]::IsNullOrWhiteSpace($Port)) {
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
        foreach ($sourceFile in $SourceFiles) {
            Copy-Item (Join-Path $SourceRoot $sourceFile) $stagingSketch
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
                & $invokeBuild "Upload" $productionSketch
            }

            if ($null -ne $testError) {
                throw $testError
            }
        }
    } finally {
        Remove-Item $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}