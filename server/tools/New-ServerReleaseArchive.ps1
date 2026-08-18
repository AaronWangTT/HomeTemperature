[CmdletBinding()]
param(
    [string]$Revision = "HEAD",

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryRoot = (
    Resolve-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
).Path
$resolvedOutput = if ([System.IO.Path]::IsPathRooted($OutputPath)) {
    [System.IO.Path]::GetFullPath($OutputPath)
} else {
    [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputPath))
}
$outputDirectory = Split-Path -Parent $resolvedOutput
$auditDirectory = Join-Path ([System.IO.Path]::GetTempPath()) (
    "home-temperature-release-" + [guid]::NewGuid().ToString("N")
)

if (Test-Path -LiteralPath $resolvedOutput) {
    throw "Output archive already exists: $resolvedOutput"
}

New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $auditDirectory | Out-Null

Push-Location $repositoryRoot
try {
    $commitOutput = (& git rev-parse --verify "$Revision^{commit}" 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $commitOutput -notmatch "^[0-9a-f]{40}$") {
        throw "Unable to resolve Git revision '$Revision'."
    }
    $commit = $commitOutput

    $treeEntries = @(& git ls-tree -r $commit -- server 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to inspect the server tree at $commit."
    }
    if ($treeEntries -match "^120000\s") {
        throw "The server release tree contains a symbolic link."
    }

    $shellTreeEntries = @($treeEntries | Where-Object { $_ -match "`tserver/.+\.sh$" })
    if ($shellTreeEntries.Count -eq 0) {
        throw "The server release tree contains no shell scripts."
    }
    foreach ($treeEntry in $shellTreeEntries) {
        $metadata, $path = $treeEntry -split "`t", 2
        $mode = ($metadata -split " ", 2)[0]
        if ($mode -ne "100755") {
            throw "Shell script is not executable in Git revision ${commit}: $path"
        }
    }

    & git archive --format=tar.gz "--output=$resolvedOutput" $commit -- server
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to create the server release archive."
    }
    $entries = @(& tar -tzf $resolvedOutput)
    if ($LASTEXITCODE -ne 0 -or $entries.Count -eq 0) {
        throw "Unable to list the server release archive."
    }

    $requiredEntries = @(
        "server/Caddyfile",
        "server/DEPLOY.md",
        "server/Dockerfile",
        "server/compose.yaml",
        "server/requirements.txt",
        "server/app/asgi.py",
        "server/scripts/backup.sh",
        "server/scripts/bootstrap-ubuntu.sh",
        "server/scripts/deploy.sh",
        "server/tests/run-deploy-preflight-tests.sh",
        "server/tools/New-ServerReleaseArchive.ps1"
    )
    foreach ($requiredEntry in $requiredEntries) {
        if ($entries -notcontains $requiredEntry) {
            throw "Release archive is missing $requiredEntry."
        }
    }

    foreach ($entry in $entries) {
        $normalized = $entry.Replace("\", "/").TrimEnd("/")
        if (
            [string]::IsNullOrWhiteSpace($normalized) -or
            $normalized.StartsWith("/") -or
            $normalized -match "(^|/)\.\.($|/)"
        ) {
            throw "Release archive contains an unsafe path: $entry"
        }
        if ($normalized -notmatch "^server(?:/|$)") {
            throw "Release archive contains a path outside server/: $entry"
        }

        $isEnvironmentExample = $normalized -eq "server/.env.example"
        $isForbidden = (
            ($normalized -match "(^|/)\.env(?:\..*)?$") -or
            ($normalized -match "(^|/)(?:\.venv|__pycache__|\.pytest_cache|backups)(?:/|$)") -or
            ($normalized -match "\.(?:db|sqlite)(?:-(?:wal|shm))?$") -or
            ($normalized -match "\.(?:pyc|pyo)$")
        )
        if ($isForbidden -and -not $isEnvironmentExample) {
            throw "Release archive contains a forbidden runtime path: $entry"
        }
    }

    & tar -xzf $resolvedOutput -C $auditDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to extract the release archive for auditing."
    }

    $shellScripts = @(
        Get-ChildItem -LiteralPath (Join-Path $auditDirectory "server") -Filter "*.sh" -File -Recurse
    )
    if ($shellScripts.Count -eq 0) {
        throw "Release archive contains no shell scripts to audit."
    }

    $expectedShebang = [Text.Encoding]::ASCII.GetBytes("#!/usr/bin/env bash`n")
    foreach ($shellScript in $shellScripts) {
        $bytes = [System.IO.File]::ReadAllBytes($shellScript.FullName)
        if ($bytes -contains 13) {
            throw "Shell script contains a carriage return: $($shellScript.Name)"
        }
        if ($bytes.Length -lt $expectedShebang.Length) {
            throw "Shell script is too short: $($shellScript.Name)"
        }
        for ($index = 0; $index -lt $expectedShebang.Length; $index++) {
            if ($bytes[$index] -ne $expectedShebang[$index]) {
                throw "Shell script has an invalid LF shebang: $($shellScript.Name)"
            }
        }
    }

    $archive = Get-Item -LiteralPath $resolvedOutput
    $checksum = (Get-FileHash -LiteralPath $resolvedOutput -Algorithm SHA256).Hash.ToLowerInvariant()
    [pscustomobject]@{
        Revision = $commit
        Archive = $archive.FullName
        Files = $treeEntries.Count
        Entries = $entries.Count
        ShellScripts = $shellScripts.Count
        SizeBytes = $archive.Length
        Sha256 = $checksum
    }
} catch {
    if (Test-Path -LiteralPath $resolvedOutput) {
        Remove-Item -LiteralPath $resolvedOutput -Force
    }
    throw
} finally {
    Pop-Location
    Remove-Item -LiteralPath $auditDirectory -Recurse -Force -ErrorAction SilentlyContinue
}