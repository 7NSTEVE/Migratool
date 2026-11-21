param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\dist")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

$srcZipPath = Join-Path $OutDir "Migratool-2.1-src-clean.zip"
$evidZipPath = Join-Path $OutDir "Migratool-2.1-evidence.zip"

if (Test-Path $srcZipPath) { Remove-Item $srcZipPath -Force }
if (Test-Path $evidZipPath) { Remove-Item $evidZipPath -Force }

$excludedRootDirs = @(
    ".git",
    ".migratool",
    ".serena",
    ".vs",
    ".vscode",
    "backups",
    "build",
    "out",
    "qa",
    "release"
)

$excludedSubDirs = @(
    "bin",
    "obj",
    ".serena",
    ".migratool",
    "__pycache__"
)

$srcZip = [System.IO.Compression.ZipFile]::Open($srcZipPath, [System.IO.Compression.ZipArchiveMode]::Create)
$srcEntries = 0
try {
    $allFiles = Get-ChildItem -Path $RepoRoot -Recurse -File -Force
    foreach ($file in $allFiles) {
        $rel = $file.FullName.Substring($RepoRoot.Length).TrimStart('\', '/')
        $parts = $rel -split '[\\/]'
        if ($parts.Length -eq 0) { continue }
        if ($excludedRootDirs -contains $parts[0]) { continue }
        $skip = $false
        foreach ($seg in $parts) {
            if ($excludedSubDirs -contains $seg) {
                $skip = $true
                break
            }
        }
        if ($skip) { continue }
        if ($file.Extension -in @(".user", ".suo", ".tmp", ".bak", ".zip")) { continue }

        $entryName = ($rel -replace '\\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $srcZip,
            $file.FullName,
            $entryName,
            [System.IO.Compression.CompressionLevel]::Optimal
        ) | Out-Null
        $srcEntries++
    }
} finally {
    $srcZip.Dispose()
}

$evidZip = [System.IO.Compression.ZipFile]::Open($evidZipPath, [System.IO.Compression.ZipArchiveMode]::Create)
$evidEntries = 0
try {
    $evidenceFiles = @(
        @{ Path = (Join-Path $RepoRoot "build\migratool-1.0-benchmark.json"); Entry = "benchmarks/migratool-1.0-benchmark.json" },
        @{ Path = (Join-Path $RepoRoot "build\migratool-2.0-competitor-benchmark.json"); Entry = "benchmarks/migratool-2.0-competitor-benchmark.json" },
        @{ Path = (Join-Path $RepoRoot "build\migratool-1.0-competitor-benchmark.json"); Entry = "benchmarks/migratool-1.0-competitor-benchmark.json" },
        @{ Path = (Join-Path $RepoRoot "build\delivery\Testing\Temporary\LastTest.log"); Entry = "ctest/LastTest.log" },
        @{ Path = (Join-Path $RepoRoot "release\stable\bin\stable-manifest.json"); Entry = "release/stable-manifest.json" },
        @{ Path = (Join-Path $RepoRoot "CHANGELOG.md"); Entry = "docs/CHANGELOG.md" }
    )
    foreach ($item in $evidenceFiles) {
        if (Test-Path $item.Path) {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $evidZip,
                $item.Path,
                $item.Entry,
                [System.IO.Compression.CompressionLevel]::Optimal
            ) | Out-Null
            $evidEntries++
        }
    }
} finally {
    $evidZip.Dispose()
}

$srcInfo = Get-Item $srcZipPath
$evidInfo = Get-Item $evidZipPath

$manifest = [ordered]@{
    generated_at_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    source_package = [ordered]@{
        path = $srcZipPath
        size_bytes = $srcInfo.Length
        size_kb = [math]::Round($srcInfo.Length / 1024.0, 2)
        entries_count = $srcEntries
        sha256 = (Get-FileHash -Path $srcZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
        excludes = $excludedRootDirs
    }
    evidence_package = [ordered]@{
        path = $evidZipPath
        size_bytes = $evidInfo.Length
        size_kb = [math]::Round($evidInfo.Length / 1024.0, 2)
        entries_count = $evidEntries
        sha256 = (Get-FileHash -Path $evidZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

$manifestPath = Join-Path $OutDir "package-manifest.json"
$manifest | ConvertTo-Json -Depth 6 | Set-Content -Path $manifestPath -Encoding UTF8
Write-Output ($manifest | ConvertTo-Json -Depth 6)
