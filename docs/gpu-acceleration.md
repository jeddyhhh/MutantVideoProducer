# GPU acceleration

Mutant Video Producer prefers available hardware for decoding, preview/export compositing,
speech inference, RVC conversion and H.264/HEVC/AV1 encoding. Each stage has its
own fallback; a GPU compositor does not imply every stage uses the GPU.

## Graphics and export

Editable graphics scenes (versions 1 and 2), waveforms, titles, captions, credits
and timecode can paint directly into OpenGL textures. Paths, text outlines,
gradients, masks, particles, trails and shadow/glow strokes use GPU rasterization.
Scene evaluation, font shaping and path preparation still use the CPU. A scene's
own pictures stay on the GPU too: motion blur moments (the scene's and a layer's)
are summed in a float framebuffer, soft layers (blur, depth of field) are blurred
and given their own effects by the GPU effects, and groups seen through as one
(opacity or a blend) are drawn into framebuffers. Blurred title shadows still
use CPU images. The preview paints scenes on the GPU frame by frame; without GPU
compositing, a look-ahead draws the next frames on the CPU's other cores.
A 348-layer episode scene with motion blur measured 235 ms per 1080p frame on
the CPU and 68 ms on the GPU (preview at 960x540: 80 ms with the CPU look-ahead,
35 ms on the GPU).
Compound timelines render recursively into GPU textures with separate decoder,
generator and effect caches per instance, avoiding intermediate image readbacks.
Effects without a GPU implementation retain their CPU path.

Same-aspect exports to 8-bit YUV420P/NV12 use GPU RGB-to-YUV conversion with the
appropriate BT.709/BT.601 and full/limited range. A packed readback transfers
1.5 bytes/pixel instead of RGBA's 4 bytes/pixel. Chroma uses a 2x2 average.
Other pixel formats and aspect reframing use the existing CPU conversion.
The render window and MCP status identify the conversion backend; GPU conversion
time is included in composition time. This is not zero-copy hardware encoding:
encoder input and hardware-decoder transfers still pass through host memory.

MCP `render` prefers a usable hardware encoder by default. Set
`hardware_encoding: false` to preserve the template's encoder. GUI rendering
defaults to hardware encoding on a fresh configuration; saved user preferences
remain respected. Explicit GPU templates still select their hardware encoder.

## Offline speech and RVC

The optional Windows Speech Generation CUDA option downloads CUDA ONNX Runtime. Install
it through Tools > AI Components. A pinned full-precision
Kokoro model is selected when installed; compact INT8 remains the CPU fallback.
Download Selected Voice + Model installs both models. The CUDA/cuDNN libraries
are shared with the RVC Conversion CUDA environment, so users do not install Python or a CUDA
Toolkit. Voice generation logs the actual backend, and MCP `voice_status`
returns `backends`. GPU initialization/inference failure retries on the CPU.
Install both Speech Generation and RVC Conversion for shared CUDA libraries. Speech
remains usable on CPU when only its own engine is installed.

RVC moves its voice network, HuBERT and RMVPE to CUDA after testing a real kernel.
It uses full precision and preserves the existing safe checkpoint loader.
FAISS retrieval, pronunciation, resampling and audio file handling stay on CPU.
Whisper transcription already has a Vulkan backend with CPU fallback.

MCP `generate_voice` accepts `device: auto|gpu|cpu` (default auto). The RVC object
also accepts its own `device`. Both auto and gpu prefer GPU with CPU fallback.
`MVP_AI_DEVICE=cpu` forces CPU workers when no explicit request overrides it.
For diagnostics, `MVP_CPU_GENERATORS=1` retains the previous CPU generator
path, `MVP_CPU_LOOKAHEAD=1` keeps the preview's CPU look-ahead for scenes,
`MVP_CPU_COLOR=1` retains swscale conversion, and `MVP_CPU_FLOW=1` works out
the motion for Optical Flow and Reduce Noise on the CPU again. These switches
do not disable the rest of GPU compositing.

On the GPU too: the animated backgrounds and overlays (Plasma and Aurora as
shaders of their own, the rest painted by the GPU), the motion between frames
for Optical Flow slow motion and Reduce Noise (a block-matching pyramid in
shaders: no frames read back), effects made in the Effect Builder and its
preview (the Composite Signal nodes too: the signal stays on the card as a float
texture; only the TV's sync tracking, a pass over the samples in order, and the
Enhancer's circuit run on the CPU), and the Live tab's whole frame (the layer with its effects, the Live FX,
a pad switch's transition and the pads) with one upload and one read back.

## Packaging

`scripts/build-rvc-worker.ps1` packages CUDA 12.8 PyTorch by default; `-CpuOnly`
produces the smaller CPU distribution. `scripts/build-voice-worker.ps1` packages
CUDA ONNX Runtime. Ship both portable workers with a GPU-capable build. Their
dependencies and license notices stay in `_internal`; do not copy just the EXEs.

## Validation on the development RTX 5070

The route-animation benchmark rendered 90 HD frames in approximately 1.0 s
with direct GPU painting versus 11.1 s with CPU generation and GPU upload.
This measures one animation fixture, not a whole-documentary speedup.
A 5-second 1920x1080/30 fps documentary excerpt exported through MCP in 3.24 s
using h264_nvenc and GPU colour conversion, with 150 frames and a successful
full decode check. The representative frame was checked against CPU output.
199 motion, graphics, compound/project and voice regression checks passed.
Portable-worker and MCP tests confirmed CUDA speech and CUDA RVC on the actual
GPU, with valid audio and CPU fallback checks. Model validation/loading,
phonemization and initial resampling/pitch preparation still contribute to
voice-job startup time; inference acceleration does not eliminate that work.
