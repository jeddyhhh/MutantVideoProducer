# Includes CUDA and CPU inference; users do not need Python, CUDA Toolkit or RVC installed.
param([string]$Python = "python", [string]$Output = "deps/rvc-worker", [switch]$CpuOnly)
$ErrorActionPreference = 'Stop'
$taskTemp = Join-Path $PSScriptRoot '../build/tmp'
New-Item -ItemType Directory -Force $taskTemp | Out-Null
$env:TEMP = (Resolve-Path $taskTemp).Path
$env:TMP = $env:TEMP
& $Python -m venv deps/rvc-worker-env
if ($LASTEXITCODE) { throw 'Could not create RVC build environment' }
$workerPython = Join-Path $PSScriptRoot '../deps/rvc-worker-env/Scripts/python.exe'
$torchBuild = if ($CpuOnly) { 'cpu' } else { 'cu128' }
& $workerPython -m pip install --no-cache-dir --upgrade "torch==2.7.1+$torchBuild" --index-url "https://download.pytorch.org/whl/$torchBuild"
if ($LASTEXITCODE) { throw 'Could not install PyTorch' }
& $workerPython -m pip install -r (Join-Path $PSScriptRoot '../data/rvc/requirements.txt')
if ($LASTEXITCODE) { throw 'Could not install RVC dependencies' }
& $workerPython -m PyInstaller --noconfirm --clean --onedir --name mvp-rvc-worker --distpath $Output --workpath build/rvc-worker --specpath build/rvc-worker --paths (Join-Path $PSScriptRoot '../data/rvc/vendor') --collect-all librosa --collect-all faiss --collect-all parselmouth --collect-submodules infer --collect-submodules tools --collect-submodules transformers.models.hubert --recursive-copy-metadata transformers --recursive-copy-metadata librosa (Join-Path $PSScriptRoot '../data/rvc/worker.py')
if ($LASTEXITCODE) { throw 'Could not package RVC worker' }
$internal = Join-Path $Output 'mvp-rvc-worker/_internal'
# Preserve third-party notices even for packages whose PyInstaller hook omits metadata.
Get-ChildItem (Join-Path $PSScriptRoot '../deps/rvc-worker-env/Lib/site-packages') -Directory -Filter '*.dist-info' | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $internal -Recurse -Force
}
$basePython = & $workerPython -c "import sys; print(sys.base_prefix)"
Copy-Item -LiteralPath (Join-Path $basePython 'LICENSE.txt') -Destination (Join-Path $internal 'PYTHON-LICENSE.txt') -Force
