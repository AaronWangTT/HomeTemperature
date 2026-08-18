param(
    [ValidateSet("Verify", "Run")]
    [string]$Action = "Verify",

    [string]$Port
)

$ErrorActionPreference = "Stop"

$sketchRoot = Join-Path (Split-Path -Parent $PSScriptRoot) "AZ3166"
$sourceRoot = Join-Path $sketchRoot "src"
$testSource = Join-Path $PSScriptRoot "LocalWebServerTests\LocalWebServerTests.ino"
. (Join-Path $PSScriptRoot "Az3166TestHarness.ps1")

Invoke-Az3166TestSuite `
    -SketchRoot $sketchRoot `
    -SourceRoot $sourceRoot `
    -SuiteName "LocalWebServerTests" `
    -TestSource $testSource `
    -SourceFiles @(
        "AppConfig.h",
        "LocalWebServer.h",
        "LocalWebServer.cpp",
        "FloatFormatting.cpp",
        "TelemetryService.h",
        "TelemetryService.cpp"
    ) `
    -Action $Action `
    -Port $Port