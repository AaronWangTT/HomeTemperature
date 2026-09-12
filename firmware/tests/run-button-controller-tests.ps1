param(
    [ValidateSet("Verify", "Run")]
    [string]$Action = "Verify",

    [string]$Port
)

$ErrorActionPreference = "Stop"

$sketchRoot = Join-Path (Split-Path -Parent $PSScriptRoot) "AZ3166"
$sourceRoot = Join-Path $sketchRoot "src"
$testSource = Join-Path $PSScriptRoot "ButtonControllerTests\ButtonControllerTests.ino"
. (Join-Path $PSScriptRoot "Az3166TestHarness.ps1")

Invoke-Az3166TestSuite `
    -SketchRoot $sketchRoot `
    -SourceRoot $sourceRoot `
    -SuiteName "ButtonControllerTests" `
    -TestSource $testSource `
    -SourceFiles @(
        "config/AppConfig.h",
        "input/ButtonDebouncer.h",
        "input/ButtonDebouncer.cpp",
        "input/ButtonController.h",
        "input/ButtonController.cpp"
    ) `
    -StageSourcesUnderSrc `
    -Action $Action `
    -Port $Port