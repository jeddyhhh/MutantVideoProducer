# Building Mutant Video Producer

Mutant Video Producer is C++20 on top of [FFmpeg](https://ffmpeg.org) (all media decoding and encoding) and
[Qt 6](https://www.qt.io) (the UI). It builds on Windows and Linux.

## Windows

Requirements: Visual Studio 2022 or newer with the C++ x64 toolset (the Build Tools are enough),
and CMake and Ninja on the PATH (Visual Studio's "C++ CMake tools for Windows", or
`python -m pip install cmake ninja`). Qt and FFmpeg live in `deps\` (not in git), the same
versions the release builds use:

```bat
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -m qtmultimedia -O deps\Qt
rem FFmpeg 8.1 shared dev build (headers + import libs + DLLs) into deps\ffmpeg, checksum verified:
pwsh -File scripts\download-ffmpeg.ps1
rem The verified uv helper that Tools > AI Components uses to install the speech and RVC engines:
pwsh -File scripts\download-ai-bootstrap.ps1
build.bat
```

The scripts need PowerShell 7 (`pwsh`). The result is `build\Release\bin\MutantVideoProducer.exe` with
all its DLLs, the user guide (`docs`) and the example projects (`examples`) next to it.

Qt older than 6.8 does not build with Visual Studio 2026: its headers use
`stdext::checked_array_iterator`, which that compiler removed.

### Optional parts

Each is left out of the build when its folder is missing.

- **Auto Captions**: `deps\whisper`, the [whisper.cpp](https://github.com/ggml-org/whisper.cpp)
  v1.9.5 source. It is built into Mutant Video Producer; the speech models are downloaded by the app when first used.
- **Whisper on the GPU**: the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows), installed
  in `C:\VulkanSDK`. Whisper is then built with its Vulkan backend. People using Mutant Video Producer need only
  a graphics driver; without one, it listens on the CPU.
- **Remove Background (AI)**: `deps\onnxruntime`, ONNX Runtime with DirectML. The models are in
  `data\models`. From the NuGet packages `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 and
  `Microsoft.AI.DirectML` 1.15.4 (rename `.nupkg` to `.zip`):
  - `build\native\include\*` goes in `deps\onnxruntime\include`,
  - `runtimes\win-x64\native\*` and `bin\x64-win\DirectML.dll` go in `deps\onnxruntime\lib`.
- **VST3 Plugin**: `deps\vst3\pluginterfaces`:
  `git clone https://github.com/steinbergmedia/vst3_pluginterfaces deps\vst3\pluginterfaces`.

## Linux

```sh
sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev qt6-multimedia-dev libqt6opengl6-dev libgl-dev \
     libavdevice-dev libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev libswscale-dev libswresample-dev
./build.sh
```

FFmpeg 6.0 or newer and Qt 6.4 or newer are required (Ubuntu 24.04+, Debian 13+, Fedora 39+). The
optional parts in `deps/` work the same way (whisper.cpp builds anywhere). On Linux, Create Clip from
YouTube uses `yt-dlp` and `ffmpeg` from the PATH (or downloads yt-dlp the first time); Spout, the OBS
virtual camera, Remove Background on DirectML and the installer are Windows-only.

`.github/workflows/linux-build.yml` also packages each build as an AppImage with
[linuxdeploy](https://github.com/linuxdeploy/linuxdeploy) and its Qt plugin (pinned versions,
checksums verified): Qt with its plugins and FFmpeg go inside, and the program's data stays next to
it in `usr/bin`. The workflow starts it off-screen with `--screenshot` as a smoke test. Version tags
attach the AppImage and the tar.gz to the release.

## Releases

GitHub Actions builds them (`.github/workflows/windows-release.yml`). Pushing a `v*` tag,
or manually running the workflow against that tag, builds and attaches:

- `MutantVideoProducer-<version>-win64-setup.exe`: the installer, made with [Inno Setup](https://jrsoftware.org/isinfo.php)
  from `installer/mvp.iss`. It installs for the current user (or, if chosen, all users), adds
  Start menu and optional desktop shortcuts, opens `.mvp` files with Mutant Video Producer, and has an uninstaller.
  To build it by hand: `iscc /DAppVersion=0.9.3 /DSourceDir=<the release folder> installer\mvp.iss`.
- `MutantVideoProducer-<version>-win64.zip`,
  The standard installer and portable ZIP omit the speech/RVC frameworks and enforce
  a size budget below 1 GiB. Optional engines download directly from upstream sources
  through Tools > AI Components; Python setup is automatic.
- `MutantVideoProducer-<version>-win64-offline.zip`: milestones only. A full edition with
  both frozen engines and Whisper models for computers without internet (several GB, split
  into parts). Tag pushes leave it out; for a milestone, run the workflow manually on the
  version tag with **offline** ticked. That run builds the frozen engines too and attaches
  everything to the same release.

Pushes to main (and manual runs on other branches) store tested builds in draft development releases,
independently of the Actions artifact quota. `scripts/package-windows.py` creates the
small layout without requiring frozen workers. `scripts/download-ai-bootstrap.ps1`
fetches pinned official uv with archive/executable checksums; the workflow verifies a
real CPU speech environment before packaging. The installer has disk spanning disabled
and must remain below 1 GiB: users download only the single setup EXE. Large optional
offline ZIPs may be split for release storage.
An already-tested installer can also be published by promoting its draft release;
publishing a release itself does not start another build. Release notes identify
which platform downloads are included and whether the installer was built locally
or by Actions.
`.github/workflows/linux-build.yml` builds Mutant Video Producer on Ubuntu 24.04 (Qt 6.4 and FFmpeg from its own
packages) and runs the self-tests that need no GPU, on every push and pull request.

The version comes from the tag: the workflow sets `MVP_VERSION` (v0.2.5 gives 0.2.5), and
`CMakeLists.txt` uses its `project(... VERSION ...)` otherwise. Help > About shows it, and the update
check compares it with the latest release. The program icon is `data/mvp.ico` (`data/mvp.rc`);
`data/mvp-icon.png` is embedded in the app for window/taskbar icons on both platforms.
The asset sizes and generation prompt are recorded in [icon provenance](icon.md).

## Documentation and example projects

- The user guide is the Markdown in `docs/`. The build copies it next to the program, where
  **Help > User Guide** shows it. Help > Tutorials lists `docs/tutorials/*.md` by file name.
- The tutorials' example projects are in `data/examples`, made by
  `mvp_selftest examples data/examples narration.wav` (see [Testing](testing.md#example-projects)).
  Run it again after a change to the project format.

## Licences

- Mutant Video Producer itself is GPL-3.0-or-later (`LICENSE`).
- The bundled FFmpeg build is GPL (it includes x264 and x265), which fits Mutant Video Producer's own licence.
- Qt is used under the LGPL-3.0 (its libraries are shipped as separate DLLs, which can be replaced).
- whisper.cpp and ONNX Runtime are MIT licensed. DirectML.dll is redistributed under its Microsoft
  licence (`DIRECTML-LICENSE.txt`).
- The MediaPipe selfie segmentation model in `data/models` is Apache-2.0. RobustVideoMatting
  (`data/models/rvm_mobilenetv3_fp32.onnx`) is GPL-3.0.
- The VST3 interface headers are MIT. VST is a trademark of Steinberg Media Technologies GmbH.
- The sound effects in `data/sfx` (copied to `sfx` next to the program) are CC0: made for Mutant Video Producer, or
  from Kenney, OpenGameArt.org and the U.S. Fish & Wildlife Service; `data/sfx/CREDITS.md` lists each one's source.
