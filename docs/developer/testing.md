# Testing

See also [Adversarial hardening campaigns](../adversarial-testing.md) and [Local voice worker development](voice.md).

## Portable project collection

`mvp_collection_test` creates isolated fixtures and tests duplicate basenames, deduplication, nested media, narration takes, RVC models, LUTs, animated images, image variables and symbol overrides. It moves the collected folder and hides the originals, checks picture/audio output, saves and moves it again, and checks missing-file reports, cancellation cleanup, existing-destination protection and sources changing during copying. The editor suite also exercises `collect_project` through MCP and its progress window.

```
build/Release/bin/mvp_collection_test
```

For only the collection UI/MCP checks, set `MVP_COLLECTION_ONLY=1` and run `MutantVideoProducer --editor-test <output.log> <testmedia/h264_aac.mp4>`. The fixture checks an abandoned HTTP client during the progress dialog and verifies the server remains responsive. Unset the variable for the full editor regression.

## Commands

```sh
tests/make_test_media.sh deps/ffmpeg/bin/ffmpeg.exe testmedia
build/Release/bin/mvp_selftest probe testmedia/*
build/Release/bin/mvp_selftest export testmedia/h264_aac.mp4 build/exporttest
build/Release/bin/mvp_selftest effects testmedia/h264_aac.mp4 build/fxtest
build/Release/bin/mvp_selftest generators build/gentest
build/Release/bin/mvp_selftest speed testmedia/h264_aac.mp4 build/speedtest
build/Release/bin/mvp_selftest gpu testmedia build/gputest
build/Release/bin/mvp_selftest audiofx testmedia build/afxtest
build/Release/bin/mvp_selftest envelopes testmedia build/envtest
build/Release/bin/mvp_selftest ramps testmedia build/ramptest
build/Release/bin/mvp_selftest racks testmedia build/racktest
build/Release/bin/mvp_selftest editing testmedia build/edittest
build/Release/bin/mvp_selftest beats
build/Release/bin/mvp_selftest beatstress
build/Release/bin/mvp_selftest tools testmedia build/toolstest
build/Release/bin/mvp_selftest random testmedia/h264_aac.mp4 build/randomtest
build/Release/bin/mvp_selftest presets testmedia build/presettest
build/Release/bin/mvp_selftest consoles testmedia build/consoletest
build/Release/bin/mvp_selftest live build/livetest
build/Release/bin/mvp_selftest captions speech.wav ggml-tiny.bin [dialogue.wav ggml-small.en-tdrz.bin]
build/Release/bin/mvp_selftest vst3 "C:/Program Files/Common Files/VST3/SomePlugin.vst3"
build/Release/bin/mvp_selftest youtube yt-dlp.exe "https://youtu.be/<a video over a minute long>"
build/Release/bin/mvp_selftest docs docs data/examples src
build/Release/bin/mvp_selftest examples build/examples narration.wav
build/Release/bin/mvp_selftest docanimations data/examples docs/animations
build/Release/bin/mvp_selftest harden testmedia data/examples build/harden [iterations]
build/Release/bin/MutantVideoProducer --live-test build/live_test.txt
build/Release/bin/MutantVideoProducer --devices-test build/devices_test.txt testmedia/h264_aac.mp4
build/Release/bin/MutantVideoProducer --editor-test build/editor_test.txt testmedia/h264_aac.mp4
build/Release/bin/MutantVideoProducer --monkey-test build/monkey.txt [--steps 3000] [--seed 1] [--live] <a copy of an example project>.mvp
```

Each prints `PASS` / `FAIL` lines and exits with the number of failures. The `MutantVideoProducer --...-test`
runs write their results to the given file, and use their own settings and folders, not yours.

If a file of yours fails to import, `mvp_selftest probe <file>` prints what was detected.

## What each suite checks

- **probe**: each file opens, decodes video at several positions (forward, far jumps, backwards
  seeks, frame stepping) and produces non-silent audio.
- **export**: renders a timeline through every template and re-imports the result (image sequences:
  counts the frames; HLS / DASH: checks the playlist and segments), checks each picture has the
  template's size and that size-limited files fit, and tests fitting a file size, the four framing
  modes, re-targeting a template to the GPU encoder, and the hvc1 tag on HEVC.
- **effects**: renders every effect and transition (contact sheets `effects.png` and
  `transitions.png`), checks keyframes, track and output FX, project save / load and an export with effects.
- **generators**: renders every text and background preset (`generators.png`), checks the
  animations, compositing, save / load and export of titles.
- **speed**: frame mapping at 200% and in reverse (and reverse decoding speed), frame blending,
  splitting sped-up events, Pan/Crop geometry and keyframes, and the pitch of time-stretched versus
  tape-style audio.
- **gpu**: renders about 45 scenarios (every effect and transition, Pan/Crop, speed, titles, odd
  sources) on the GPU and CPU renderers and requires them to match; checks hardware decoding against
  software decoding; benchmarks CPU against GPU at 1080p and 4K.
- **audiofx**: runs every audio effect and measures what it does (filter attenuation, EQ gain,
  compressor and limiter levels, gate, noise reduction, pitch, effect tails, sync of delayed effects
  with the picture, master FX in exports).
- **envelopes**: volume, pan and composite envelopes in playback and exports.
- **ramps**: the ramp maths; that video, GPU, splits and trims follow the ramp; freezes and
  backwards ramps; presets; and that audio clicks land on time along a ramp (pitch-preserved and
  tape-style), at the right pitch, without dropouts, and silent during freezes.
- **racks**: chain editing (reorder, group, move into and out of racks, copies get new ids), nested
  racks in projects, video blend modes, opacity, solo and dry / wet on the CPU and GPU, and summed
  audio chains with delay compensation.
- **editing**: the ripple operations; titles dropped inside an event; removing a crossfade over an
  event inside another; looping past the end of a file; cutting instead of crossfading; track moves;
  markers, regions and compositing modes saved; autosave and recovery; relinking; every track blend
  mode on the CPU and GPU; fade curve shapes (equal power); auto ducking; per-track meters;
  freeze-frame rendering; `.cube` parsing; Curves, Color Wheels and LUT on the CPU and GPU.
- **beats**: the beat tracker on synthetic drum loops at 90, 128, 140 and 174 BPM (tempo within
  0.35 BPM, beat grid within 8 ms); every-hit mode; a tempo change; BPM ranges; hi-hats; latency
  compensation; silence; Lock BPM; ÷2; Set 1; Find Tempo Again; a lead vocal over boom bap; and
  2-minute songs that used to fool it.
- **beatstress**: the longer report behind that: ten arranged songs in three BPM ranges, how much of
  the time the tempo is right, song changes, and every song again with a lead vocal on top.
- **tools**: Detect Beats on a 120 BPM loop (within 30 ms), bar markers, Split at Markers, a Beat
  Mutant Video Producer; effect masks (ellipse, outside, a rotated feathered rectangle the same on the GPU);
  stabilisation on a picture shaken by a known amount; Track Motion on a square moving along a curve
  (within 4 px); proxies of several heavy formats (frame times, colours, speed, cancelling, the
  cache); the scopes on colour bars and a ramp.
- **random**: 30 random effect chains per tier: sizes, every effect and setting exists and is in
  range, no repeats, at most one slow effect, how much each tier changes a picture, variety, and that
  a seed always gives the same chain.
- **presets**: every factory preset (known effects, settings in range, FFmpeg chains that build,
  audible and stable output), delay compensation, and saving, reloading, overwriting and deleting
  user presets.
- **consoles**: every Broken Video Game Systems effect over a run of frames on the CPU and GPU (they
  must match, with no CPU fallback), and one labelled sheet per console (`console_<name>.png`).
- **live**: capture sources, the recorder, audio capture, picture / sound sync, modulation, video
  files as a source, the replay buffer, MIDI clock, scenes, overlays and setlists, portrait framing
  and zoom, streaming to an RTMP server on this computer, Spout, and the OBS Virtual Camera.
- **captions**: Whisper on a recording of speech and, given a dialogue and the speakers model,
  speaker turns.
- **vst3**: loads an installed plugin, runs a tone through it, saves and reloads its state, opens
  its window (never shown) and plays it on a track.
- **youtube**: reads times like 1:30, downloads a video with picture and sound, cancels, and reports
  a wrong link with yt-dlp's message.
- **docs**: every link and picture in the user guide points at something that exists (and, given
  `src`, every page the program opens: Help buttons, Shift+F1, the welcome window), every
  tutorial is listed in the tutorials index, and every example project a tutorial opens exists.
- **examples**: makes the example projects (see below), loads each back, and renders eight frames
  along each.
- **docanimations**: renders the user guide's animations (`docs/animations`) from the example
  projects with the GIF template, running Track Motion on the effects example first.
- **harden** tries to break the core the way real files and users do, and needs no crash, hang
  or silent damage. Damaged media: each test file emptied, cut short at several points, a run of
  zeros in the middle, bytes flipped, the right header then noise, and a file cut short or deleted
  after it was placed. Odd but valid media made with FFmpeg: 1x1, 3x7, 4000x16, a single frame, 1
  and 1000 fps, a variable frame rate, a late start, one audio sample, 384 kHz with 8 channels, ten
  minutes of silence, two video streams, a 12000x8000 still, 16-bit pictures, and names in other
  scripts and with symbols. Mangled project files (`iterations` of them, from the examples): values
  replaced by ones out of range or of the wrong type, keys removed, the file cut short or scribbled
  over, events repeated; each must be refused with a message or load sound (see
  `timelineProblem()`), render, edit, and save and load back the same. Random editing: every kind
  of edit with odd arguments, checked after each step, then all of it undone and redone step by
  step. Export edge cases: empty and backwards ranges, sizes from 0x0 to 9000x9000, zero and
  negative frame rates, missing folders, a folder's name, no codecs, an unknown encoder, nonsense
  options, cancelling (no half file left), over a file the project uses (refused) and over a
  read-only file. Six threads rendering and mixing at once. Memory over 3000 frames. A project of
  about 1700 events with effects: edit time, undo memory per step, saving, loading and rendering.
  Mangled `.cube` LUTs and SRT / WebVTT files. A crash or hang names the stage and its seed;
  `MVP_FUZZ_SEED=<seed>` runs one mangled project again, `MVP_HARDEN_ONLY=<part>` one part
  (damaged, odd, projects, editing, export, parallel, memory, big, parsers).
- **MutantVideoProducer --monkey-test** runs random menu commands, keys, clicks, drags and wheel turns on the
  Editor (off-screen) for `--steps` steps, answering the dialogs they open at random, and checks the
  timeline after every step. It leaves out what would leave the test or touch your things (quitting,
  links, downloads, cameras, exports, the Live tab), and fails if any window of its own shows on the
  screen. Each step is written to the log as it happens; a hang (a minute without a step) adds a
  report with the stuck stack, and a crash leaves the usual crash report. With `--live` it works on
  the Live tab instead (pads, scenes, the setlist, the switcher, the layer, recording and replays,
  with the picture running): the project's videos are its only sources and a test beat its only
  sound, never a camera, microphone, screen or window of the computer (`MVP_NO_DEVICES=1`);
  going on air and the virtual camera stay off-limits, and recordings go to the test's folder.
- **MutantVideoProducer --editor-test** drives the Editor with real mouse and keyboard events: markers and
  regions, ripple edits, transitions, slip, slide, roll, copies, looping, snapping, track moves,
  J / K / L, clicking the timeline while playing, autosave and recovery, Open Recent, relinking, box
  and time selection, nudging, freeze frames, fade curves, the Mixer, Copy Frame, the Properties tab
  and its handles on the preview, proxies, the scopes, the media bin, History, shortcuts, captions,
  VST3, Create Clip from YouTube's placement, the Help window and example projects, thumbnails that
  follow the file (not the media id), File > New's shapes, the welcome window, the system check, the
  update check's version comparison, Shift+F1 and dialogs' Help buttons, the sound output reopened
  while playing, and AI control (MCP over HTTP and stdio, and every tool called with arguments of
  the wrong kind or far out of range, plus malformed requests).
- **MutantVideoProducer --devices-test** drives the device chain with real drag-and-drop and mouse events,
  masks on the preview, and freehand point editing.
- **MutantVideoProducer --live-test** drives the Live tab: capture, effects, pads, transitions, pad sets, auto
  switch, audio beats and tempo locking, MIDI, layers, overlays, replays, launch, scenes, the three
  layouts, setlists, Spout and the virtual camera, cue and take, portrait sizes, zoom and pan,
  streaming, random chains, and recording into From Live.

## Environment variables

- `MVP_TEST_PORTRAIT=<head-and-shoulders photo>`: `tools` and `gpu` also check Remove Background
  on a real person (both models, both renderers).
- `MVP_SEGMENT_CPU=1` and `MVP_WHISPER_CPU=1` keep those models off the GPU.
- `MutantVideoProducer --crash-test segv|abort` crashes on purpose, to check the crash report (written to its own
  `MvpCrashTest` folder).
- `MVP_NO_DEVICES=1` lists no cameras, screens, windows, microphones or desktop audio on the Live
  tab (video files and a 120 BPM test beat only); the monkey test sets it.
- `MVP_EXAMPLES_PREVIEW=<folder>`: `examples` also saves a contact sheet of each project there.

## Example projects

`mvp_selftest examples data/examples narration.wav` writes the tutorials' example projects and
their media (music, a sample clip, and the narration) into `data/examples`. The projects are built
with the same model the Editor edits, saved with paths relative to their folder, then loaded back
and rendered. Run it again after a change to the project format.

The narration is read out by Windows' speech synthesiser:

```powershell
Add-Type -AssemblyName System.Speech
$tts = New-Object System.Speech.Synthesis.SpeechSynthesizer
$tts.SelectVoice('Microsoft Zira Desktop'); $tts.Rate = -1
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(48000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Stereo)
$tts.SetOutputToWaveFile("$PWD\narration.wav", $fmt)
$tts.Speak("Welcome to Mutant Video Producer. This narration comes with the audio and captions tutorials. Listen to the music while I speak: it gets quieter, and when I stop, it comes back up. That is auto ducking.")
$tts.Speak("Now try Auto Captions on this track. Every sentence I say becomes a caption on the timeline, timed to my voice. You can restyle the captions, move them, or export them as subtitles.")
$tts.Dispose()
```
