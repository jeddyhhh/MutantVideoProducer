# Builds a portable worker; no Python installation is needed on the user's PC.
param([string]$Python = "python", [string]$Output = "deps/voice-worker")
$ErrorActionPreference = 'Stop'
$taskTemp = Join-Path $PSScriptRoot '../build/tmp'
New-Item -ItemType Directory -Force $taskTemp | Out-Null
$env:TEMP = (Resolve-Path $taskTemp).Path
$env:TMP = $env:TEMP
& $Python -m venv deps/voice-worker-env
if ($LASTEXITCODE) { throw 'Could not create voice build environment' }
$workerPython = Join-Path $PSScriptRoot '../deps/voice-worker-env/Scripts/python.exe'
& $workerPython -m pip install -r (Join-Path $PSScriptRoot '../data/voice/requirements.txt') pyinstaller==6.21.0
if ($LASTEXITCODE) { throw 'Could not install voice dependencies' }
# kokoro-onnx declares the CPU distribution, which shares the same import
# directory with GPU ORT. Install exactly one final runtime to avoid mixed DLLs.
& $workerPython -m pip uninstall -y onnxruntime
& $workerPython -m pip install --force-reinstall --no-deps onnxruntime-gpu==1.24.4
if ($LASTEXITCODE) { throw 'Could not install GPU voice runtime' }
& $workerPython -m PyInstaller --noconfirm --clean --onedir --name mvp-voice-worker --distpath $Output --workpath build/voice-worker --specpath build/voice-worker --collect-all kokoro_onnx --collect-all espeakng_loader --collect-all phonemizer --copy-metadata kokoro-onnx (Join-Path $PSScriptRoot '../data/voice/worker.py')
if ($LASTEXITCODE) { throw 'Could not package voice worker' }
