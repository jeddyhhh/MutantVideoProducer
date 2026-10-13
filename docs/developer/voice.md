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

Verification:

```powershell
python -m unittest discover -s tests -p test_voice_worker.py
.\build\Release\bin\mvp_voice_test.exe
```

The C++ integration test needs the packaged worker and offline Kokoro assets. It
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
