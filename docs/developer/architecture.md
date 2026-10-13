# Architecture

```
src/core/      no UI dependencies; linked by the app and the self-test
  FFmpeg       FFmpeg includes, RAII wrappers, version-portable capability queries
  MediaInfo    probe a file: streams, duration, rotation, aspect, stills
  VideoDecoder frame-accurate random access (forward decode, keyframe seek, fallbacks)
  AudioDecoder sample-accurate access, resampled to stereo float
  Project      tracks / events / media pool, snapshot-based undo, JSON save/load
  TimelineOps  pure edit operations (insert, split, ripple delete, group...)
  Compositor   renders a timeline frame: event FX -> transitions -> track FX -> output FX
  Effects      effect/transition model, keyframes, parameter resolution, JSON
  EffectMask   effect masks (rectangle, ellipse, freehand) shared by both renderers
  VideoEffects / Transitions   built-in implementations (CPU, multithreaded)
  TransitionKernels  the per-pixel transitions (Glitch, Cube Spin, Film Burn...), written once
               in C++ and once in GLSL with the same uniforms, so both renderers agree
  GlitchEffects  glitch, analog and psychedelic effects; per-row/random "plans" shared with
               the GPU shaders (GpuGlitchShaders.h) so both renderers agree
  ConsoleEngine / ConsoleEffects   "Broken Video Game Systems": a virtual console screen
               (resolution, palette, dither, CRT/LCD) driven by per-row, per-column and per-tile
               tables plus a triangle-mesh stage; the GPU shader (GpuConsoleShaders.h) runs the
               same per-pixel program on the same packed tables
  Segmentation Remove Background (AI): the person models on ONNX Runtime + DirectML
  ImageOps     pixel kernels (LUTs, colour matrices, separable box/Gaussian blur, mask blends)
  FilterGraph  libavfilter bridge for the FFmpeg Filter effect
  Generators   generated media: text layout/rendering, credits, timecode, backgrounds, presets
  GeneratorsAnimated  animated backgrounds, overlays, kinetic text, countdown, colour bars
               (each frame worked out from the time alone)
  PanCrop      event framing transform (fit/fill/stretch, zoom, centre, rotation, flips)
  Stabilizer / MotionTracker   shake analysis and correction; Track Motion
  AudioTempo   pitch-preserving time-stretch (FFmpeg atempo) for sped-up/slowed audio
  AudioStretch WSOLA time-stretch that follows a speed ramp sample-accurately
  FrameRenderer  interface implemented by Compositor (CPU) and GpuCompositor (OpenGL)
  GpuCompositor  the OpenGL renderer: YUV textures + shaders, same output as the CPU path
  Hardware     GPU settings, shared hardware decoder device, hardware encoder detection
  AudioEffects audio DSP (biquad EQ/filters, dynamics, Freeverb, delays, modulation) and
               FFmpeg-filter effects with measured, compensated latency
  Vst3Host     loads VST3 plugins, runs them, saves their state
  AudioMixer   mixes audio tracks (fades, crossfades, volume, pan, solo/mute, ducking)
  BeatTracker / BeatMarkers   tempo and beat tracking; Detect Beats, Beat Montage
  Captions     Whisper transcription, SRT / WebVTT
  Exporter     encodes the timeline with any muxer/encoder (framing, size caps, image
               sequences, subtitle tracks); ExportPresets: the render templates
  Proxy        proxy files for heavy footage
  Scopes       waveform, RGB parade, vectorscope, histogram
  YouTube      Create Clip from YouTube (runs yt-dlp)
  Live*        the Live tab: capture (LiveCapture: FFmpeg devices, gfxcapture), audio input
               (LiveAudio, WASAPI loopback), recording (LiveRecorder), modulation, pad sets, MIDI
src/engine/
  PlaybackEngine  video render thread + audio mixer thread; audio-clock sync
  LiveEngine   Live tab: capture + recording + a render thread applying the Live FX chain
               (FrameRenderer::renderLive), the switcher's inputs
  MediaCache   background thumbnails and waveforms
src/ui/        Qt widgets: timeline, preview, media bin, device chain, dialogs, the Live tab, the themes
               (Theme: every colour comes from theme::c(), so View > Theme switches live),
               the MCP server (AI control), the help window
tests/
  selftest.cpp        the self-test suites (see Testing), and the example projects
  make_test_media.sh  generates ~65 test clips in modern, legacy and odd formats
docs/          this guide (copied next to the program for Help > User Guide)
data/models    the Remove Background models
data/examples  the tutorials' example projects and their media
```

## Threads and snapshots

Render and playback threads never touch the live project. They work on immutable
`ProjectSnapshot`s, so editing during playback is safe. Every edit makes a new timeline and an undo
step (`Project::applyEdit`).

## Two renderers, one result

The CPU `Compositor` and the OpenGL `GpuCompositor` implement the same `FrameRenderer` interface and
must give the same picture. Effects that are naturally sequential use parallel formulations on the
GPU that give the same result as the CPU: Pixel Sort a bitonic sorting network, error-diffusion
dithering diagonal waves in integer maths, Databend's Echo a doubling scan. The `gpu` self-test
renders about 45 scenarios on both and requires them to match.

## CPU effects and shared images

CPU effects work in place on a `QImage`, usually from several threads (`parallelFor`). Before an
effect runs, the compositor makes sure the image's memory is not shared: a shared image would be
copied ("detached") by each thread's `scanLine()` at once. `Compositor::applyChain` detaches it
once on its own thread whenever it keeps a copy for a mask or a glitch post-mask.
