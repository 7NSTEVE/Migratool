param(
    [string]$BuildBinDir = "",
    [string]$StableDir   = (Join-Path (Split-Path -Parent $PSScriptRoot) "release\stable\bin")
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($BuildBinDir)) {
    $candDelivery = Join-Path $RepoRoot "build\delivery\bin"
    $candV2       = Join-Path $RepoRoot "build\v2\bin"
    $exeDel = Join-Path $candDelivery "migratool-cli-0.3.exe"
    $exeV2  = Join-Path $candV2 "migratool-cli-0.3.exe"
    if ((Test-Path $exeDel) -and (Test-Path $exeV2)) {
        if ((Get-Item $exeDel).LastWriteTimeUtc -ge (Get-Item $exeV2).LastWriteTimeUtc) {
            $BuildBinDir = $candDelivery
        } else {
            $BuildBinDir = $candV2
        }
    } elseif (Test-Path $exeDel) {
        $BuildBinDir = $candDelivery
    } else {
        $BuildBinDir = $candV2
    }
}

$requiredFiles = @(
    "migratool-cli-0.3.exe",
    "Migratool-0.3.exe",
    "sqlite3.dll",
    "adapters\roslyn\RoslynAdapter.dll"
)

foreach ($rel in $requiredFiles) {
    $srcPath = Join-Path $BuildBinDir $rel
    if (-not (Test-Path $srcPath)) {
        throw "Falta binario requerido antes de publicar estable: $srcPath"
    }
}

$stagingDir = "$StableDir.staging-$([Guid]::NewGuid().ToString('N').Substring(0,8))"
New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stagingDir "adapters\roslyn") | Out-Null

Copy-Item (Join-Path $BuildBinDir "migratool-cli-0.3.exe") (Join-Path $stagingDir "migratool-cli-0.3.exe") -Force
Copy-Item (Join-Path $BuildBinDir "migratool-cli-0.3.exe") (Join-Path $stagingDir "migratool-cli.exe") -Force
Copy-Item (Join-Path $BuildBinDir "Migratool-0.3.exe")     (Join-Path $stagingDir "Migratool-0.3.exe") -Force
Copy-Item (Join-Path $BuildBinDir "Migratool-0.3.exe")     (Join-Path $stagingDir "Migratool.exe") -Force
Copy-Item (Join-Path $BuildBinDir "sqlite3.dll")           (Join-Path $stagingDir "sqlite3.dll") -Force
foreach ($optDll in @("libclang.dll", "libzip.dll", "zip.dll")) {
    $optSrc = Join-Path $BuildBinDir $optDll
    if (Test-Path $optSrc) {
        Copy-Item $optSrc (Join-Path $stagingDir $optDll) -Force
    }
}
Copy-Item (Join-Path $BuildBinDir "adapters\roslyn\*")     (Join-Path $stagingDir "adapters\roslyn") -Recurse -Force

$artifacts = @()
Get-ChildItem -Path $stagingDir -Recurse -File | ForEach-Object {
    $hash = (Get-FileHash -Path $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $rel  = $_.FullName.Substring($stagingDir.Length).TrimStart('\', '/')
    $artifacts += [ordered]@{
        relative_path = $rel.Replace('\', '/')
        bytes         = $_.Length
        sha256        = $hash
    }
}

$manifest = [ordered]@{
    schema       = "migratool.stable-release/1"
    published_at = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    source_bin   = $BuildBinDir.Replace('\', '/')
    stable_bin   = $StableDir.Replace('\', '/')
    artifacts    = $artifacts
}

$manifestPath = Join-Path $stagingDir "stable-manifest.json"
$manifest | ConvertTo-Json -Depth 6 | Set-Content -Path $manifestPath -Encoding UTF8

New-Item -ItemType Directory -Force -Path $StableDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StableDir "adapters\roslyn") | Out-Null
Copy-Item (Join-Path $stagingDir "*") $StableDir -Recurse -Force
Add-Type -AssemblyName Microsoft.VisualBasic
[Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($stagingDir, 'OnlyErrorDialogs', 'SendToRecycleBin')

Write-Output ($manifest | ConvertTo-Json -Depth 6)
