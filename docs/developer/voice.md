# Local caption-to-speech

`core/Voice` owns the pinned catalog, asset verification, sentence grouping and
result validation. `ui/VoiceJob` starts a separate process and imports validated
WAV takes onto one audio track. The dialog and MCP share this job implementation.
It snapshots timeline, project settings and project path, and refuses insertion if
they change during generation. Windows job objects stop the worker and RVC children
on cancellation. Worker requests and WAV files remain local.

`data/voice/worker.py` uses kokoro-onnx with CPU ONNX Runtime and bundled eSpeak NG.
It accepts one request JSON path and writes progress JSON lines and an atomic
`result.json`. Timing fit is represented by pitch-preserving clip speeds in the
editor, rather than destructive resampling. The bundled RVC worker runs between
TTS and timing calculation. All phrases are converted in one batch, so shared
models are loaded once. An explicit `engine: "external"` retains the legacy CLI
adapter for developer integrations; the UI and MCP default to bundled conversion.

Build the portable Windows worker before building the editor:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-voice-worker.ps1
powershell -ExecutionPolicy Bypass -File scripts/build-rvc-worker.ps1
.\build.bat Release
```

For an offline voice build, first run `python scripts/download-voice-assets.py`.
It downloads every catalog voice and the shared model, checking the pinned hashes.
For offline RVC support, also run:
`python scripts/download-voice-assets.py --catalog data/rvc/catalog.json --output deps/rvc-assets`.
The build copies the RVC engine from `deps/rvc-worker/mvp-rvc-worker` into
`bin/voice/rvc-worker` and shared assets into `bin/voice/assets/rvc`. The pinned
official inference modules and model catalog are in `data/rvc`.

Pre-rename portable worker bundles are also accepted and deployed with the new
executable names; see [settings and worker compatibility](../rename.md).

The build copies `data/voice` into `bin/voice`, and copies a packaged worker from
`deps/voice-worker/mvp-voice-worker` when present. It also copies optional offline
model/voice assets from `deps/voice-assets`, preserving the catalog's relative
paths. Validate all downloaded assets against `data/voice/catalog.json` before
redistribution. Model licenses and source links are in `data/voice/NOTICE.md`.

Without offline assets, the editor downloads selected assets to its local app data
directory. `MVP_VOICE_DIR` overrides that cache for tests. Bundled assets are a
fallback when an asset is absent from the cache. `MVP_VOICE_WORKER` overrides
the executable for diagnostics. RVC uses a separate runtime to avoid dependency
conflicts with the lightweight Kokoro worker.

The standard installer omits both frozen worker directories. Its small verified
`runtime/uv.exe` helper installs private Python 3.12.12 from Astral, then uses
`data/runtime/locks` to sync hash-locked binary wheels from PyPI/PyTorch. Speech and
RVC have separate environments because their NumPy versions differ. The speech CUDA
overlay replaces CPU ONNX Runtime; its libraries can be shared from CUDA RVC.

`core/AiRuntime` creates each environment at its permanent per-user generation path.
Windows venvs cannot simply be renamed. After successful import checks, a QSaveFile
atomically publishes the active pointer; failed/cancelled setup removes only its new
generation and retains the previous pointer. The marker pins dependency fingerprints
and the interpreter checksum. Worker launches use isolated Python (`-I`) with the
installed app's scripts. RVC receives an explicit program/argument prefix, preserving
support for bundled executables. `MVP_AI_RUNTIME_DIR` and `MVP_AI_CATALOG` isolate tests.
Never change the dependency inputs without regenerating locks and catalog checksums.
Run `python scripts/lock-ai-runtimes.py` after preparing the pinned uv helper with
`scripts/download-ai-bootstrap.ps1`. Use `MVP_BUNDLE_AI=OFF` when building the lean
edition locally; full/offline builds keep the default `ON`.

Verification:

```powershell
python -m unittest discover -s tests -p test_voice_worker.py
.\build\Release\bin\mvp_voice_test.exe
```

The C++ integration test needs either an installed upstream environment or the packaged worker, and offline Kokoro assets. It
generates real speech, mixes it through the editor, checks fit/overruns, undo/redo,
project save/load, cancellation, missing runtime and stale-project protection.
Python tests check checksum verification, timing and legacy RVC CLI contracts.
`tests/rvc_reference_smoke.py` runs real CPU RMVPE and Praat conversion with the
official pretrained v2 f0G40k generator, checking audible output and duration.
This verifies inference, not any fine-tuned identity voice's quality. Give the
packaged executable as its argument to verify portable runtime packaging.
Set `MVP_RVC_TEST_MODEL` to that test's exported reference checkpoint to also
exercise the complete caption-to-RVC-to-timeline flow in `mvp_voice_test.exe`.
Imported voices use `torch.load(weights_only=True)` without an unsafe fallback.
