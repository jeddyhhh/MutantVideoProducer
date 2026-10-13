# Building Mutant Video Producer

Mutant Video Producer is C++20 on top of [FFmpeg](https://ffmpeg.org) (all media decoding and encoding) and
[Qt 6](https://www.qt.io) (the UI). It builds on Windows and Linux.

## Windows

Requirements: Visual Studio 2019 or 2022 with the C++ x64 toolset (the Build Tools are enough).
Qt and FFmpeg live in `deps\` (not in git):

```bat
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 6.5.3 win64_msvc2019_64 -m qtmultimedia -O deps\Qt
rem FFmpeg shared dev build (headers + import libs + DLLs), unpacked to deps\ffmpeg:
rem https://github.com/BtbN/FFmpeg-Builds/releases  ->  ffmpeg-n8.1-latest-win64-gpl-shared-8.1.zip
build.bat
```

The result is `build\Release\bin\MutantVideoProducer.exe` with all its DLLs, the user guide (`docs`) and the
example projects (`examples`) next to it.

With Visual Studio 2026 use Qt 6.8 or newer (`6.8.3 win64_msvc2022_64`): older Qt headers use
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

## Releases

GitHub Actions builds them (`.github/workflows/windows-release.yml`). Publishing a release with a
new `v*` tag (or pushing such a tag) attaches:

- `MutantVideoProducer-<version>-win64-setup.exe`: the installer, made with [Inno Setup](https://jrsoftware.org/isinfo.php)
  from `installer/mvp.iss`. It installs for the current user (or, if chosen, all users), adds
  Start menu and optional desktop shortcuts, opens `.mvp` files with Mutant Video Producer, and has an uninstaller.
  To build it by hand: `iscc /DAppVersion=0.2.5 /DSourceDir=<the release folder> installer\mvp.iss`.
- `MutantVideoProducer-<version>-win64.zip`,
- `MutantVideoProducer-<version>-win64-offline.zip`: the same with every Whisper speech model in
  `models\whisper` (about 700 MB more), for computers without internet.

Every push to a `claude/...` branch also builds a test zip, downloadable from that run's *Artifacts*.
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
