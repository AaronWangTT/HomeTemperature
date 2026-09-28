param(
    [ValidateSet("Verify", "Run")]
    [string]$Action = "Verify",
    [string]$Port
)

$ErrorActionPreference = "Stop"
$sketchRoot = Join-Path (Split-Path -Parent $PSScriptRoot) "AZ3166"
$sourceRoot = Join-Path $sketchRoot "src"
$testSource = Join-Path $PSScriptRoot "LocalOtaControllerTests\LocalOtaControllerTests.ino"
. (Join-Path $PSScriptRoot "Az3166TestHarness.ps1")

Invoke-Az3166TestSuite `
    -SketchRoot $sketchRoot `
    -SourceRoot $sourceRoot `
    -SuiteName "LocalOtaControllerTests" `
    -TestSource $testSource `
    -SourceFiles @(
        "http/LocalHttpHandler.h",
        "http/LocalHttpStreamingHandler.h",
        "ota/LocalOtaController.h",
        "ota/LocalOtaController.cpp",
        "ota/LocalOtaHttpHandler.h",
        "ota/LocalOtaHttpHandler.cpp",
        "ota/LocalOtaPlatform.h",
        "connectivity/NetworkMaintenanceCoordinator.h",
        "connectivity/NetworkMaintenanceCoordinator.cpp",
        "ota/OtaRebootCoordinator.h"
    ) `
    -StageSourcesUnderSrc `
    -Action $Action `
    -Port $Port
