param(
    [string]$BuildBinDir    = "",
    [string]$StableDir      = (Join-Path (Split-Path -Parent $PSScriptRoot) "release\stable\bin"),
    [string]$ExpectedCommit = "",
    [string]$Version        = "3.1.0",
    [switch]$AllowDirty
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName Microsoft.VisualBasic
$RepoRoot = Split-Path -Parent $PSScriptRoot

$gitExe = "git"
if (Test-Path "D:\DevTools\Tools\Git\cmd\git.exe") {
    $gitExe = "D:\DevTools\Tools\Git\cmd\git.exe"
}

$headSha = ""
$workingTreeClean = $false
try {
    $headOut = & $gitExe -C $RepoRoot rev-parse HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $headOut) {
        $headSha = ($headOut | Select-Object -First 1).Trim()
    }
    $statusOut = & $gitExe -C $RepoRoot status --porcelain 2>$null
    if ($LASTEXITCODE -eq 0) {
        $workingTreeClean = [string]::IsNullOrWhiteSpace(($statusOut -join "`n"))
    }
} catch {
    $headSha = ""
    $workingTreeClean = $false
}

if (-not $AllowDirty -and -not $workingTreeClean) {
    throw "PUBLISH_BLOCKED_DIRTY_TREE: el árbol de trabajo tiene cambios sin commit. Haga commit primero o pase -AllowDirty explícitamente."
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedCommit)) {
    if ([string]::IsNullOrWhiteSpace($headSha) -or -not $headSha.StartsWith($ExpectedCommit, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "PUBLISH_BLOCKED_COMMIT_MISMATCH: HEAD actual '$headSha' no coincide con -ExpectedCommit '$ExpectedCommit'."
    }
}

if ([string]::IsNullOrWhiteSpace($BuildBinDir)) {
    $candDelivery = Join-Path $RepoRoot "build\delivery\bin"
    if (Test-Path (Join-Path $candDelivery "migratool-cli-0.3.exe")) {
        $BuildBinDir = $candDelivery
    } else {
        throw "PUBLISH_BLOCKED_MISSING_BUILD: no se encontró build\delivery\bin\migratool-cli-0.3.exe. Pase -BuildBinDir explícitamente o ejecute cmake --build --preset release."
    }
}

$BuildBinDir = [System.IO.Path]::GetFullPath($BuildBinDir)
$StableDir   = [System.IO.Path]::GetFullPath($StableDir)

$requiredFiles = @(
    "migratool-cli-0.3.exe",
    "Migratool-0.3.exe",
    "sqlite3.dll",
    "adapters\roslyn\RoslynAdapter.dll"
)

foreach ($rel in $requiredFiles) {
    $srcPath = Join-Path $BuildBinDir $rel
    if (-not (Test-Path -LiteralPath $srcPath -PathType Leaf)) {
        throw "Falta binario requerido antes de publicar estable: $srcPath"
    }
}

$stableParent = Split-Path -Parent $StableDir
if (-not (Test-Path -LiteralPath $stableParent)) {
    New-Item -ItemType Directory -Force -Path $stableParent | Out-Null
}

$txId       = [Guid]::NewGuid().ToString('N').Substring(0, 12)
$stagingDir = "$StableDir.staging-$txId"
$backupDir  = "$StableDir.backup-$txId"

New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stagingDir "adapters\roslyn") | Out-Null

try {
    Copy-Item -LiteralPath (Join-Path $BuildBinDir "migratool-cli-0.3.exe") -Destination (Join-Path $stagingDir "migratool-cli-0.3.exe") -Force
    Copy-Item -LiteralPath (Join-Path $BuildBinDir "migratool-cli-0.3.exe") -Destination (Join-Path $stagingDir "migratool-cli.exe") -Force
    Copy-Item -LiteralPath (Join-Path $BuildBinDir "Migratool-0.3.exe")     -Destination (Join-Path $stagingDir "Migratool-0.3.exe") -Force
    Copy-Item -LiteralPath (Join-Path $BuildBinDir "Migratool-0.3.exe")     -Destination (Join-Path $stagingDir "Migratool.exe") -Force
    Copy-Item -LiteralPath (Join-Path $BuildBinDir "sqlite3.dll")           -Destination (Join-Path $stagingDir "sqlite3.dll") -Force

    $optionalRuntimeDlls = @(
        "libclang.dll", "libzip.dll", "zip.dll",
        "zlib1.dll", "bz2.dll", "zstd.dll", "lzma.dll",
        "libbz2-1.dll", "libzstd.dll", "liblzma-5.dll"
    )
    foreach ($optDll in $optionalRuntimeDlls) {
        $optSrc = Join-Path $BuildBinDir $optDll
        if (Test-Path -LiteralPath $optSrc -PathType Leaf) {
            Copy-Item -LiteralPath $optSrc -Destination (Join-Path $stagingDir $optDll) -Force
        }
    }
    Copy-Item -Path (Join-Path $BuildBinDir "adapters\roslyn\*") -Destination (Join-Path $stagingDir "adapters\roslyn") -Recurse -Force

    $artifacts = @()
    $expectedHashes = @{}
    Get-ChildItem -LiteralPath $stagingDir -Recurse -File | Sort-Object FullName | ForEach-Object {
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $rel  = $_.FullName.Substring($stagingDir.Length).TrimStart('\', '/').Replace('\', '/')
        $expectedHashes[$rel] = $hash
        $artifacts += [ordered]@{
            relative_path = $rel
            bytes         = $_.Length
            sha256        = $hash
        }
    }

    # Pre-activation staging integrity verification
    foreach ($kv in $expectedHashes.GetEnumerator()) {
        $stagedPath = Join-Path $stagingDir ($kv.Key.Replace('/', '\'))
        $actualHash = (Get-FileHash -LiteralPath $stagedPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $kv.Value) {
            throw "STAGING_HASH_MISMATCH: $($kv.Key) expected=$($kv.Value) actual=$actualHash"
        }
    }

    $manifest = [ordered]@{
        schema             = "migratool.stable-release/2"
        version            = $Version
        git_commit_sha     = $headSha
        working_tree_clean = $workingTreeClean
        activation_mode    = "atomic_directory_swap_with_rollback"
        published_at       = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        source_bin         = $BuildBinDir.Replace('\', '/')
        stable_bin         = $StableDir.Replace('\', '/')
        artifacts          = $artifacts
    }

    $manifestPath = Join-Path $stagingDir "stable-manifest.json"
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

    # Atomic swap: StableDir -> BackupDir, StagingDir -> StableDir, then post-swap verification with rollback on any error
    $hadPrevious = Test-Path -LiteralPath $StableDir
    $movedPrevious = $false
    $activatedNew  = $false
    try {
        if ($hadPrevious) {
            [System.IO.Directory]::Move($StableDir, $backupDir)
            $movedPrevious = $true
        }
        [System.IO.Directory]::Move($stagingDir, $StableDir)
        $activatedNew = $true

        foreach ($kv in $expectedHashes.GetEnumerator()) {
            $livePath = Join-Path $StableDir ($kv.Key.Replace('/', '\'))
            if (-not (Test-Path -LiteralPath $livePath -PathType Leaf)) {
                throw "POST_SWAP_MISSING_FILE: $livePath"
            }
            $liveHash = (Get-FileHash -LiteralPath $livePath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($liveHash -ne $kv.Value) {
                throw "POST_SWAP_HASH_MISMATCH: $($kv.Key) expected=$($kv.Value) actual=$liveHash"
            }
        }
    } catch {
        $origError = $_
        if ($activatedNew -and (Test-Path -LiteralPath $StableDir)) {
            $failedDir = "$StableDir.failed-$txId"
            try { [System.IO.Directory]::Move($StableDir, $failedDir) } catch {}
        }
        if ($movedPrevious -and (Test-Path -LiteralPath $backupDir) -and -not (Test-Path -LiteralPath $StableDir)) {
            [System.IO.Directory]::Move($backupDir, $StableDir)
        }
        throw "PUBLISH_ATOMIC_SWAP_FAILED (rollback restaurado=$($movedPrevious)): $($origError.Exception.Message)"
    }

    if ($movedPrevious -and (Test-Path -LiteralPath $backupDir)) {
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($backupDir, 'OnlyErrorDialogs', 'SendToRecycleBin')
    }

    Write-Output ($manifest | ConvertTo-Json -Depth 6)
} catch {
    if (Test-Path -LiteralPath $stagingDir) {
        try {
            [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($stagingDir, 'OnlyErrorDialogs', 'SendToRecycleBin')
        } catch {}
    }
    throw
}
