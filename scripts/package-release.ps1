param(
    [string]$RepoRoot  = (Split-Path -Parent $PSScriptRoot),
    [string]$OutDir    = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\dist"),
    [string]$Version   = "3.1.0",
    [string]$CommitRef = "HEAD",
    [switch]$AllowDirty
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName Microsoft.VisualBasic

$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)
$OutDir   = [System.IO.Path]::GetFullPath($OutDir)

if (-not (Test-Path -LiteralPath $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

$gitExe = "git"
if (Test-Path "D:\DevTools\Tools\Git\cmd\git.exe") {
    $gitExe = "D:\DevTools\Tools\Git\cmd\git.exe"
}

$commitSha = ((& $gitExe -C $RepoRoot rev-parse "$CommitRef^{commit}") | Select-Object -First 1)
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($commitSha)) {
    throw "PACKAGE_GIT_REF_INVALID: no se pudo resolver '$CommitRef' en $RepoRoot"
}
$commitSha = $commitSha.Trim()

$treeSha = ((& $gitExe -C $RepoRoot rev-parse "$CommitRef^{tree}") | Select-Object -First 1)
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($treeSha)) {
    throw "PACKAGE_GIT_TREE_INVALID: no se pudo resolver árbol de '$CommitRef'"
}
$treeSha = $treeSha.Trim()

$dirtyOut = & $gitExe -C $RepoRoot status --porcelain
if ($LASTEXITCODE -ne 0) {
    throw "PACKAGE_GIT_STATUS_FAILED en $RepoRoot"
}
$workingTreeClean = [string]::IsNullOrWhiteSpace(($dirtyOut -join "`n"))
if (-not $AllowDirty -and -not $workingTreeClean) {
    throw "PACKAGE_BLOCKED_DIRTY_TREE: el repositorio tiene cambios sin commit. Haga commit primero o pase -AllowDirty explícitamente."
}

$srcZipPath  = Join-Path $OutDir "Migratool-$Version-src-clean.zip"
$evidZipPath = Join-Path $OutDir "Migratool-$Version-evidence.zip"

foreach ($oldZip in @($srcZipPath, $evidZipPath)) {
    if (Test-Path -LiteralPath $oldZip -PathType Leaf) {
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($oldZip, 'OnlyErrorDialogs', 'SendToRecycleBin')
    }
}

# Empaquetado inmutable directo desde el objeto commit de Git (jamás lee archivos modificados del working tree)
& $gitExe -C $RepoRoot archive --format=zip --output=$srcZipPath $commitSha
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $srcZipPath -PathType Leaf)) {
    throw "PACKAGE_GIT_ARCHIVE_FAILED para commit $commitSha"
}

$srcEntries = 0
$srcInspect = [System.IO.Compression.ZipFile]::OpenRead($srcZipPath)
try {
    $srcEntries = ($srcInspect.Entries | Where-Object { -not $_.FullName.EndsWith("/") }).Count
} finally {
    $srcInspect.Dispose()
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
        if (Test-Path -LiteralPath $item.Path -PathType Leaf) {
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

$srcInfo  = Get-Item -LiteralPath $srcZipPath
$evidInfo = Get-Item -LiteralPath $evidZipPath

$manifest = [ordered]@{
    schema             = "migratool.release-package/2"
    version            = $Version
    git_commit_sha     = $commitSha
    git_tree_sha       = $treeSha
    working_tree_clean = $workingTreeClean
    generated_at_utc   = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    source_package     = [ordered]@{
        path             = $srcZipPath
        size_bytes       = $srcInfo.Length
        size_kb          = [math]::Round($srcInfo.Length / 1024.0, 2)
        entries_count    = $srcEntries
        sha256           = (Get-FileHash -LiteralPath $srcZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
        source_selection = "git archive $commitSha (immutable Git object)"
    }
    evidence_package   = [ordered]@{
        path          = $evidZipPath
        size_bytes    = $evidInfo.Length
        size_kb       = [math]::Round($evidInfo.Length / 1024.0, 2)
        entries_count = $evidEntries
        sha256        = (Get-FileHash -LiteralPath $evidZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

$manifestPath = Join-Path $OutDir "package-manifest.json"
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Output ($manifest | ConvertTo-Json -Depth 6)
