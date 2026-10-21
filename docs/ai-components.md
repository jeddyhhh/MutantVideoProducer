# Optional AI components (Windows)

The standard Windows download is one standalone `-setup.exe`. Run it normally to
install the editor, media libraries, graphics, audio tools and native AI backends.
There are no installer parts to join and no separate Python installer to run.
Speech generation, RVC frameworks and model caches are excluded to keep it below 1 GiB.
Existing generated narration plays and renders without either AI engine.

Open **Tools > AI Components...**, or **Install / Manage AI Components...** in Generate
Voice. Choose **Speech Generation** or **RVC Conversion**, select CPU or NVIDIA CUDA,
and click **Download & Install / Repair**. The app downloads private Python from
[Astral's managed distribution](https://docs.astral.sh/uv/concepts/python-versions/),
dependencies from [PyPI](https://pypi.org/), and Windows PyTorch wheels from
[PyTorch](https://pytorch.org/get-started/locally/). These are upstream downloads,
not AI packs hosted in our releases, and work with private development builds too.

CPU speech is the smallest option. NVIDIA (CUDA) speech brings its own cuDNN, cuBLAS and
CUDA runtime, about 2.3 GB, and needs 6 GB free while it installs; it does not need RVC.
On an RTX 5070 it speaks a minute of narration in about 4 seconds, against about a minute on
a 4-core CPU. CUDA RVC needs 12 GB free. Both need a compatible NVIDIA driver, and no CUDA
Toolkit installation. CPU fallback remains available when acceleration cannot be used.

Setup displays its current phase and dependency download output and supports Cancel.
Dependency versions and wheel hashes are locked; source builds are disabled so users
need no compiler. An engine becomes available only after its imports are checked.
Failed or cancelled repairs keep the existing working engine. You can install again
to repair or switch between CPU and CUDA.

Engines live in per-user application data, shared across projects and profiles. They
work offline after setup and need no administrator access. Voice and model downloads
remain separate in Generate Voice, Auto Captions and Remove Background; installing
an engine does not download every model or voice. Downloads may need several GB of
disk space for the environment and its reusable cache. Removing the editor preserves
these user downloads, like projects and media.

They install to `%LOCALAPPDATA%\Mutant Video Producer\Mutant Video Producer\ai-runtimes` by default (on C: for most
people), whichever drive the editor is on. The window shows the folder and the free space there. If that drive is short
of space, press **Folder...** and choose a folder on another drive: engines then install to, and load from, a
`Mutant Video Producer AI` folder inside it. **Default** goes back. Engines installed before stay where they were and
aren't used from the new folder (their Python environments can't move): install them again, then delete the old
folder. The environment variable `MVP_AI_RUNTIME_DIR`, if set, takes the place of both. Setup checks the free space
first and says how much it needs on which drive.

MCP exposes `ai_components` with `action: "list"` for engine status and upstream
sources. Use `action: "install", component: "speech", device: "cpu"` for CPU speech,
or `component: "rvc", device: "cuda"` for CUDA RVC. Installation uses the same visible
progress window and validation. Full/offline editions retain their bundled engines.
