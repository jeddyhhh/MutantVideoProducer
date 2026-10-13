# Use a completed dated build: BtbN replaces the moving "latest" release nightly.
param([switch]$ResolveOnly)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$apiHeaders = @{ Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
if ($env:GH_TOKEN) { $apiHeaders.Authorization = "Bearer $env:GH_TOKEN" }
$releases = Invoke-RestMethod -Uri 'https://api.github.com/repos/BtbN/FFmpeg-Builds/releases?per_page=10' -Headers $apiHeaders -MaximumRetryCount 3 -RetryIntervalSec 10
$asset = $null
foreach ($release in $releases) {
    if ($release.draft -or $release.tag_name -notlike 'autobuild-*') { continue }
    if (-not ($release.assets | Where-Object { $_.name -eq 'checksums.sha256' })) { continue }
    $asset = $release.assets | Where-Object {
        $_.name -match '^ffmpeg-n8\.1[A-Za-z0-9.+_-]*-win64-gpl-shared-8\.1\.zip$' -and
        $_.digest -match '^sha256:[0-9a-f]{64}$'
    } | Select-Object -First 1
    if ($asset) { break }
}
if (-not $asset) { throw 'No completed dated FFmpeg 8.1 Windows shared build with a SHA-256 digest was found.' }
$expectedHash = $asset.digest.Substring(7)
Write-Host "FFmpeg release: $($release.tag_name) / $($asset.name)"
Write-Host "SHA-256: $expectedHash"
if ($ResolveOnly) { exit 0 }
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$dependencyRoot = Join-Path $repositoryRoot 'deps'
$destination = Join-Path $dependencyRoot 'ffmpeg'
if (Test-Path -LiteralPath $destination) { throw 'deps/ffmpeg already exists; refusing to replace it.' }
$temporaryRoot = Join-Path $repositoryRoot 'build/tmp'
New-Item -ItemType Directory -Force $temporaryRoot, $dependencyRoot | Out-Null
$archive = Join-Path $temporaryRoot $asset.name
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $archive -MaximumRetryCount 3 -RetryIntervalSec 10
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) { throw 'FFmpeg archive SHA-256 verification failed.' }
Expand-Archive -LiteralPath $archive -DestinationPath $dependencyRoot
$extracted = Join-Path $dependencyRoot ([IO.Path]::GetFileNameWithoutExtension($asset.name))
if (-not (Test-Path -LiteralPath (Join-Path $extracted 'include/libavcodec/avcodec.h'))) { throw 'FFmpeg development headers are missing from the archive.' }
Rename-Item -LiteralPath $extracted -NewName 'ffmpeg'
