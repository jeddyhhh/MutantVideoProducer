# Local voice component licenses and sources

Mutant Video Producer is GPL-3.0. The local worker bundles the following independently licensed
components. Their distributions include their license files and package metadata.

- Kokoro model and voices: Apache-2.0, [hexgrad/Kokoro-82M](https://huggingface.co/hexgrad/Kokoro-82M).
  ONNX conversion: [onnx-community/Kokoro-82M-v1.0-ONNX](https://huggingface.co/onnx-community/Kokoro-82M-v1.0-ONNX),
  pinned revision `1939ad2a8e416c0acfeecc08a694d14ef25f2231`.
- kokoro-onnx 0.6.1: MIT, [source](https://github.com/thewh1teagle/kokoro-onnx).
- ONNX Runtime 1.24.4: MIT, [source](https://github.com/microsoft/onnxruntime).
  The NVIDIA (CUDA) speech engine uses onnxruntime-gpu 1.24.4 (MIT) instead.
- NVIDIA (CUDA) speech engine only: cuDNN 9.7.1.26 (`nvidia-cudnn-cu12`), cuBLAS 12.8.3.14
  (`nvidia-cublas-cu12`), CUDA Runtime 12.8.57 (`nvidia-cuda-runtime-cu12`) and cuFFT 11.3.3.41
  (`nvidia-cufft-cu12`), © NVIDIA Corporation, proprietary. cuDNN is under the License Agreement for
  NVIDIA Software Development Kits; the others under the CUDA Toolkit End User License Agreement. Each
  package's `License.txt` holds its terms, also at [docs.nvidia.com/cuda/eula](https://docs.nvidia.com/cuda/eula/)
  and [docs.nvidia.com/deeplearning/cudnn/sla](https://docs.nvidia.com/deeplearning/cudnn/latest/reference/eula.html).
  They are not part of Mutant Video Producer's download: Tools > AI Components fetches them from PyPI,
  hash-checked, only when the CUDA option is chosen.
- NumPy 2.4.3: BSD-3-Clause, [source](https://github.com/numpy/numpy).
- phonemizer 3.4.0: GPL-3.0, [source](https://github.com/bootphon/phonemizer).
- espeakng-loader 0.2.4: [source and license](https://github.com/thewh1teagle/espeakng-loader).
- eSpeak NG: GPL-3.0, [source](https://github.com/espeak-ng/espeak-ng).
- CPython: PSF license, [source and license](https://www.python.org/downloads/source/).
- PyInstaller: GPL with the bootloader exception, [source and license](https://github.com/pyinstaller/pyinstaller).

The optional RVC conversion runs Mutant Video Producer's bundled portable CPU engine. See
`../rvc/NOTICE.md` for its sources and licenses. Third-party identity voice models
are imported by the user and are not included in this package.
