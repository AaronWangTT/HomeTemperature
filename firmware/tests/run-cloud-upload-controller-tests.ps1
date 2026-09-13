param(
    [ValidateSet("Verify", "Run")]
    [string]$Action = "Verify",

    [string]$Port
)

$ErrorActionPreference = "Stop"

$sketchRoot = Join-Path (Split-Path -Parent $PSScriptRoot) "AZ3166"
$sourceRoot = Join-Path $sketchRoot "src"
$testSource = Join-Path $PSScriptRoot "CloudUploadControllerTests\CloudUploadControllerTests.ino"
. (Join-Path $PSScriptRoot "Az3166TestHarness.ps1")

Invoke-Az3166TestSuite `
    -SketchRoot $sketchRoot `
    -SourceRoot $sourceRoot `
    -SuiteName "CloudUploadControllerTests" `
    -TestSource $testSource `
    -SourceFiles @(
        "config/AppConfig.h",
        "cloud/CloudTelemetry.h",
        "cloud/CloudTelemetry.cpp",
        "cloud/CloudUploadController.h",
        "cloud/CloudUploadController.cpp",
        "telemetry/TelemetryService.h",
        "telemetry/TelemetryService.cpp",
        "cloud/TelemetryUploadResult.h",
        "cloud/TelemetryUploader.h",
        "cloud/TelemetryUploader.cpp",
        "cloud/UploadScheduler.h",
        "cloud/UploadScheduler.cpp"
    ) `
    -StageSourcesUnderSrc `
    -Action $Action `
    -Port $Port