# Bundled RVC engine

Mutant Video Producer's portable CUDA/CPU worker includes inference modules from
[RVC-Project/Retrieval-based-Voice-Conversion-WebUI](https://github.com/RVC-Project/Retrieval-based-Voice-Conversion-WebUI),
commit `81eed5e8f68b6bed1789f682fe78cdd324495afc`, under the MIT license
(included in `vendor/LICENSE`). Mutant Video Producer adapts RMVPE's CUDA initialization to
accept the worker's explicit device/precision without importing WebUI settings.

HuBERT/ContentVec and RMVPE shared weights are downloaded from the official
[RVC model repository](https://huggingface.co/lj1995/VoiceConversionWebUI),
revision `e6d0c1a17da07c33557852f9dfa2bd44cc75737d`. Sizes and SHA-256 digests
are pinned in `catalog.json`. These shared models do not represent a selectable voice.

The worker bundles Python, PyTorch 2.7.1 with CUDA 12.8 and CPU inference,
CUDA/cuDNN runtime libraries, Transformers, NumPy, SciPy, librosa,
Numba, FAISS, Praat/Parselmouth and SoundFile. Their license files and package
metadata are distributed in the worker's `_internal` directory by PyInstaller.
No Python installation, CUDA Toolkit, RVC WebUI or network connection is needed
for inference once the shared models and an exported RVC voice are available.
CUDA acceleration needs a supported NVIDIA GPU and driver; otherwise the worker
uses the CPU. FAISS retrieval, resampling and file handling remain on the CPU.

Imported `.pth` files are loaded with PyTorch's `weights_only=True`; the worker
does not fall back to unrestricted pickle loading. Import voices and optional
matching indexes from sources whose license permits your use. The release does
not bundle third-party identity voice models.
