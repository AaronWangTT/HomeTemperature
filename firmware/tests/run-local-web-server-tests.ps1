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
        "config/AppConfig.h",
        "http/LocalWebServer.h",
        "http/LocalWebServer.cpp",
        "http/Az3166LocalWebServerOperations.h",
        "http/LocalHttpHandler.h",
        "telemetry/TelemetryHttpHandler.h",
        "telemetry/TelemetryHttpHandler.cpp",
        "telemetry/TelemetryService.h",
        "telemetry/TelemetryService.cpp"
    ) `
    -StageSourcesUnderSrc `
    -Action $Action `
    -Port $Port