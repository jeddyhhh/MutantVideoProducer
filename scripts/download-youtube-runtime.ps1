# Pinned, verified Deno runtime for yt-dlp's YouTube JavaScript challenges.
param([string]$Output = 'deps/deno')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$runtimeDir = [IO.Path]::GetFullPath((Join-Path $PWD $Output))
New-Item -ItemType Directory -Force $runtimeDir | Out-Null
$archivePath = Join-Path $runtimeDir 'deno.zip'
Invoke-WebRequest 'https://github.com/denoland/deno/releases/download/v2.9.7/deno-x86_64-pc-windows-msvc.zip' -OutFile $archivePath
if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'a0c3101b4158d1dfb7d6a78a7bf0f3de80c96bb423c152beec8beb22786f2238') {
    Remove-Item -LiteralPath $archivePath
    throw 'Deno archive checksum failed'
}
Expand-Archive -LiteralPath $archivePath -DestinationPath $runtimeDir -Force
Remove-Item -LiteralPath $archivePath
Invoke-WebRequest 'https://raw.githubusercontent.com/denoland/deno/v2.9.7/LICENSE.md' -OutFile (Join-Path $runtimeDir 'DENO-LICENSE.md')
