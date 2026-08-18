param(
    [ValidateSet("Verify", "Run")]
    [string]$Action = "Verify",

    [string]$Port
)

$ErrorActionPreference = "Stop"

$sketchRoot = Join-Path (Split-Path -Parent $PSScriptRoot) "AZ3166"
$sourceRoot = Join-Path $sketchRoot "src"
$testSource = Join-Path $PSScriptRoot "TelemetryServiceTests\TelemetryServiceTests.ino"
. (Join-Path $PSScriptRoot "Az3166TestHarness.ps1")

Invoke-Az3166TestSuite `
    -SketchRoot $sketchRoot `
    -SourceRoot $sourceRoot `
    -SuiteName "TelemetryServiceTests" `
    -TestSource $testSource `
    -SourceFiles @(
        "AppConfig.h",
        "FloatFormatting.cpp",
        "TelemetryService.h",
        "TelemetryService.cpp"
    ) `
    -Action $Action `
    -Port $Port