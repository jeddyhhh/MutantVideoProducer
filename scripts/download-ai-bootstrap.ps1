param([string]$Output = 'deps/ai-bootstrap')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
# Official Astral release, pinned independently of any private MVP build assets.
$version = '0.13.0'
$expected = '088962f9e7b7bd9ea740c04c650b2a21c8928c345bd99ac24350dc924dba656c'
New-Item -ItemType Directory -Force $Output | Out-Null
$archive = Join-Path $Output 'uv.zip'
Invoke-WebRequest "https://github.com/astral-sh/uv/releases/download/$version/uv-x86_64-pc-windows-msvc.zip" -OutFile $archive
if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Official uv archive checksum mismatch' }
Expand-Archive $archive -DestinationPath $Output -Force
$binaryHash = '2bfbe53d3cc95799e98fa08323b04844fc01d4f8c87dd5c00089e43f6554f162'
if ((Get-FileHash (Join-Path $Output 'uv.exe') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $binaryHash) { throw 'uv executable checksum mismatch' }
Invoke-WebRequest "https://raw.githubusercontent.com/astral-sh/uv/$version/LICENSE-MIT" -OutFile (Join-Path $Output 'UV-LICENSE-MIT.txt')
Invoke-WebRequest "https://raw.githubusercontent.com/astral-sh/uv/$version/LICENSE-APACHE" -OutFile (Join-Path $Output 'UV-LICENSE-APACHE.txt')
