[CmdletBinding()]
param(
    [ValidateSet("List", "Current", "Resolve")]
    [string]$Action = "List",

    [ValidatePattern("^\d+\.\d+\.\d+$")]
    [string]$Version,

    [ValidatePattern("^[0-9a-fA-F]{40}$")]
    [string]$IndexRevision,

    [switch]$Json
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($Action -eq "Resolve" -and [string]::IsNullOrWhiteSpace($Version)) {
    throw "Resolve requires -Version, for example -Version 2.0.2."
}

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..\..")).Path
$installerPath = Join-Path $repositoryRoot "firmware\tools\Install-Az3166Toolchain.ps1"
$installer = Get-Content -Raw -LiteralPath $installerPath

function Get-RequiredMatchValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,

        [Parameter(Mandatory = $true)]
        [string]$Pattern,

        [Parameter(Mandatory = $true)]
        [string]$Description,

        [Parameter(Mandatory = $true)]
        [string]$SourcePath
    )

    $match = [regex]::Match(
        $Text,
        $Pattern,
        [System.Text.RegularExpressions.RegexOptions]::Multiline
    )
    if (-not $match.Success) {
        throw "Could not read $Description from $SourcePath."
    }

    return $match.Groups["value"].Value
}

$currentVersion = Get-RequiredMatchValue `
    -Text $installer `
    -Pattern '^\$coreVersion\s*=\s*"(?<value>[^\"]+)"\s*$' `
    -Description "the current Core version" `
    -SourcePath $installerPath
$currentArchiveSha256 = Get-RequiredMatchValue `
    -Text $installer `
    -Pattern '^\$coreArchiveSha256\s*=\s*"(?<value>[0-9a-f]{64})"\s*$' `
    -Description "the current Core archive SHA-256" `
    -SourcePath $installerPath
$currentIndexRevision = Get-RequiredMatchValue `
    -Text $installer `
    -Pattern 'azureiotdevkit_tools/(?<value>[0-9a-f]{40})/package_azureboard_index\.json' `
    -Description "the current package-index revision" `
    -SourcePath $installerPath
$installerCompilerVersion = Get-RequiredMatchValue `
    -Text $installer `
    -Pattern '^\$compilerVersion\s*=\s*"(?<value>[^\"]+)"\s*$' `
    -Description "the compiler version" `
    -SourcePath $installerPath
$installerOpenOcdVersion = Get-RequiredMatchValue `
    -Text $installer `
    -Pattern '^\$openOcdVersion\s*=\s*"(?<value>[^\"]+)"\s*$' `
    -Description "the OpenOCD version" `
    -SourcePath $installerPath

$headers = @{
    Accept = "application/vnd.github+json"
    "User-Agent" = "HomeTemperature-AZ3166-version-skill"
    "X-GitHub-Api-Version" = "2022-11-28"
}

if ([string]::IsNullOrWhiteSpace($IndexRevision)) {
    $commit = Invoke-RestMethod `
        -Uri "https://api.github.com/repos/AaronWangTT/azureiotdevkit_tools/commits/maintenance" `
        -Headers $headers
    $IndexRevision = [string]$commit.sha
}
$IndexRevision = $IndexRevision.ToLowerInvariant()

$indexUrl = "https://raw.githubusercontent.com/AaronWangTT/azureiotdevkit_tools/$IndexRevision/package_azureboard_index.json"
$index = Invoke-RestMethod -Uri $indexUrl -Headers $headers
$packages = @($index.packages | Where-Object { $_.name -eq "AZ3166" })
if ($packages.Count -ne 1) {
    throw "Expected one AZ3166 package in $indexUrl; found $($packages.Count)."
}

$platforms = @($packages[0].platforms | Where-Object { $_ })
$duplicateVersions = @(
    $platforms |
        Group-Object -Property version |
        Where-Object { $_.Count -ne 1 }
)
if ($duplicateVersions.Count -gt 0) {
    throw "The index contains duplicate AZ3166 platform versions: $($duplicateVersions.Name -join ', ')."
}

$publishedVersions = @(
    $platforms |
        Sort-Object { [version]$_.version } |
        ForEach-Object { [string]$_.version }
)
$buildScriptPath = Join-Path $repositoryRoot "firmware\tools\Invoke-Az3166Build.ps1"
$buildScript = Get-Content -Raw -LiteralPath $buildScriptPath
$buildVersion = Get-RequiredMatchValue `
    -Text $buildScript `
    -Pattern '^\$coreVersion\s*=\s*"(?<value>[^\"]+)"\s*$' `
    -Description "the Core version" `
    -SourcePath $buildScriptPath
$buildArchiveSha256 = Get-RequiredMatchValue `
    -Text $buildScript `
    -Pattern '^\$coreArchiveSha256\s*=\s*"(?<value>[0-9a-f]{64})"\s*$' `
    -Description "the Core archive SHA-256" `
    -SourcePath $buildScriptPath
$buildCompilerVersion = Get-RequiredMatchValue `
    -Text $buildScript `
    -Pattern '^\$compilerVersion\s*=\s*"(?<value>[^\"]+)"\s*$' `
    -Description "the compiler version" `
    -SourcePath $buildScriptPath
$buildOpenOcdVersion = Get-RequiredMatchValue `
    -Text $buildScript `
    -Pattern '^\$openOcdVersion\s*=\s*"(?<value>[^\"]+)"\s*$' `
    -Description "the OpenOCD version" `
    -SourcePath $buildScriptPath

$ciPath = Join-Path $repositoryRoot ".github\workflows\ci.yml"
$ci = Get-Content -Raw -LiteralPath $ciPath
$ciCacheMatch = [regex]::Match(
    $ci,
    'az3166-core-(?<version>\d+\.\d+\.\d+)-index-(?<revision>[0-9a-f]{7,40})-windows'
)
if (-not $ciCacheMatch.Success) {
    throw "Could not read the AZ3166 cache identity from $ciPath."
}
$ciVersion = $ciCacheMatch.Groups["version"].Value
$ciIndexRevision = $ciCacheMatch.Groups["revision"].Value

$editorPath = Join-Path $repositoryRoot ".vscode\c_cpp_properties.json"
$editorConfiguration = Get-Content -Raw -LiteralPath $editorPath | ConvertFrom-Json
$editorVersion = Get-RequiredMatchValue `
    -Text ([string]$editorConfiguration.env.AZ3166_CORE) `
    -Pattern 'stm32f4/(?<value>\d+\.\d+\.\d+)$' `
    -Description "the editor Core version" `
    -SourcePath $editorPath
$editorCompilerVersion = Get-RequiredMatchValue `
    -Text ([string]$editorConfiguration.configurations[0].compilerPath) `
    -Pattern 'arm-none-eabi-gcc/(?<value>[^/]+)/bin/' `
    -Description "the editor compiler version" `
    -SourcePath $editorPath

$issues = [System.Collections.Generic.List[string]]::new()
foreach ($surface in @(
    @{ Name = "build Core version"; Actual = $buildVersion; Expected = $currentVersion },
    @{ Name = "build archive SHA-256"; Actual = $buildArchiveSha256; Expected = $currentArchiveSha256 },
    @{ Name = "build compiler version"; Actual = $buildCompilerVersion; Expected = $installerCompilerVersion },
    @{ Name = "build OpenOCD version"; Actual = $buildOpenOcdVersion; Expected = $installerOpenOcdVersion },
    @{ Name = "CI Core version"; Actual = $ciVersion; Expected = $currentVersion },
    @{ Name = "editor Core version"; Actual = $editorVersion; Expected = $currentVersion },
    @{ Name = "editor compiler version"; Actual = $editorCompilerVersion; Expected = $installerCompilerVersion }
)) {
    if ($surface.Actual -ne $surface.Expected) {
        $issues.Add("$($surface.Name) is '$($surface.Actual)'; expected '$($surface.Expected)'.")
    }
}
if (-not $currentIndexRevision.StartsWith($ciIndexRevision)) {
    $issues.Add("CI index identity '$ciIndexRevision' does not match '$currentIndexRevision'.")
}

$currentPlatforms = @($platforms | Where-Object { $_.version -eq $currentVersion })
if ($currentPlatforms.Count -ne 1) {
    $issues.Add("Core $currentVersion is not uniquely published in index revision $IndexRevision.")
} else {
    $currentPlatform = $currentPlatforms[0]
    $publishedChecksum = [string]$currentPlatform.checksum
    if ($publishedChecksum -ne "SHA-256:$currentArchiveSha256") {
        $issues.Add("Installer archive SHA-256 does not match the published Core $currentVersion entry.")
    }

    $compilerDependencies = @($currentPlatform.toolsDependencies | Where-Object { $_.name -eq "arm-none-eabi-gcc" })
    if ($compilerDependencies.Count -ne 1 -or $compilerDependencies[0].version -ne $installerCompilerVersion) {
        $issues.Add("Installer compiler version does not match the published Core $currentVersion entry.")
    }
    $openOcdDependencies = @($currentPlatform.toolsDependencies | Where-Object { $_.name -eq "openocd" })
    if ($openOcdDependencies.Count -ne 1 -or $openOcdDependencies[0].version -ne $installerOpenOcdVersion) {
        $issues.Add("Installer OpenOCD version does not match the published Core $currentVersion entry.")
    }
}

$firmwareReadmePath = Join-Path $repositoryRoot "firmware\README.md"
$firmwareReadme = Get-Content -Raw -LiteralPath $firmwareReadmePath
$thirdPartyPath = Join-Path $repositoryRoot "THIRD_PARTY_NOTICES.md"
$thirdParty = Get-Content -Raw -LiteralPath $thirdPartyPath
foreach ($document in @(
    @{ Path = $firmwareReadmePath; Text = $firmwareReadme },
    @{ Path = $thirdPartyPath; Text = $thirdParty }
)) {
    foreach ($value in @($currentVersion, $currentIndexRevision, $currentArchiveSha256)) {
        if (-not $document.Text.Contains($value)) {
            $issues.Add("$($document.Path) does not contain current pin '$value'.")
        }
    }
}

$current = [pscustomobject]@{
    Version = $currentVersion
    IndexRevision = $currentIndexRevision
    ArchiveSha256 = $currentArchiveSha256
    IsPublished = $publishedVersions -contains $currentVersion
    IsConsistent = $issues.Count -eq 0
    Issues = [string[]]$issues
    Surfaces = [pscustomobject]@{
        BuildVersion = $buildVersion
        CiVersion = $ciVersion
        CiIndexRevision = $ciIndexRevision
        EditorVersion = $editorVersion
        CompilerVersion = $installerCompilerVersion
        OpenOcdVersion = $installerOpenOcdVersion
    }
}

switch ($Action) {
    "List" {
        $result = [pscustomobject]@{
            IndexRevision = $IndexRevision
            IndexUrl = $indexUrl
            Count = $publishedVersions.Count
            Versions = $publishedVersions
            Current = $current
        }
    }
    "Current" {
        $result = $current
    }
    "Resolve" {
        $matches = @($platforms | Where-Object { $_.version -eq $Version })
        if ($matches.Count -ne 1) {
            throw "AZ3166 Core $Version is not uniquely published in index revision $IndexRevision."
        }

        $platform = $matches[0]
        $checksum = [string]$platform.checksum
        $checksumMatch = [regex]::Match(
            $checksum,
            '^SHA-256:(?<value>[0-9a-f]{64})$'
        )
        if (-not $checksumMatch.Success) {
            throw "AZ3166 Core $Version has an invalid SHA-256 checksum in the index."
        }

        $archiveUri = [uri]$platform.url
        if ([System.IO.Path]::GetFileName($archiveUri.AbsolutePath) -ne $platform.archiveFileName) {
            throw "AZ3166 Core $Version archive filename does not match its URL."
        }

        $dependencies = @(
            $platform.toolsDependencies |
                Where-Object { $_ } |
                ForEach-Object {
                    [pscustomobject]@{
                        Packager = [string]$_.packager
                        Name = [string]$_.name
                        Version = [string]$_.version
                    }
                }
        )
        $result = [pscustomobject]@{
            Version = [string]$platform.version
            Name = [string]$platform.name
            Architecture = [string]$platform.architecture
            IndexRevision = $IndexRevision
            IndexUrl = $indexUrl
            ArchiveUrl = [string]$platform.url
            ArchiveFileName = [string]$platform.archiveFileName
            ArchiveSize = [int64]$platform.size
            ArchiveSha256 = $checksumMatch.Groups["value"].Value.ToLowerInvariant()
            ToolDependencies = $dependencies
            Current = $current
        }
    }
}

if ($Json) {
    $result | ConvertTo-Json -Depth 8
    return
}

switch ($Action) {
    "List" {
        Write-Output "Published AZ3166 Core versions: $($result.Count)"
        Write-Output "Index revision: $($result.IndexRevision)"
        Write-Output "Current HomeTemperature version: $($current.Version)"
        Write-Output ($result.Versions -join ", ")
    }
    "Current" {
        $result | Format-List Version, IndexRevision, ArchiveSha256, IsPublished, IsConsistent
        if (-not $result.IsConsistent) {
            $result.Issues | ForEach-Object { Write-Output "- $_" }
        }
    }
    "Resolve" {
        $result | Format-List Version, Name, Architecture, IndexRevision, IndexUrl, ArchiveUrl, ArchiveFileName, ArchiveSize, ArchiveSha256
        $result.ToolDependencies | Format-Table -AutoSize
    }
}