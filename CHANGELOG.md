# Changelog

What changed in each version of Mutant Video Producer. The newest is first.

### Product rename

- Renamed the application to Mutant Video Producer, including Windows metadata,
  installer, MCP identity, documentation, tutorials and example narration.
- Source, support and update links now point to `jeddyhhh/MutantVideoProducer`.
- Existing projects, presets, profile libraries, preferences and default media/model
  folders remain compatible; isolated tests verify the settings migration.
- Command-line media imports resolve absolute paths so saving and recovery work
  when the program was started with a relative filename.

### General animation engine

- Version-2 native scenes add curved/morphing paths, arc-length following and orientation,
  parented/nested groups, cameras, masks, gradients, shadows/glow, trails and sampled motion blur.
- Character/word/line typography, reusable objects with variables, data-driven bar/line/donut
  charts, deterministic particles/repeaters and embedded audio/beat animation bindings.
- The graphics composer adds scrub/play preview, a draggable keyframe timeline, record mode,
  path control handles, group/object controls, camera/style settings, chart imports and templates.
- MCP exposes capabilities, validation, keyframe edits, audio bindings and template import/export.
  Existing version-1 graphics retain their rendering. See [Animation scenes](docs/animation-scenes.md).

### Native graphics and waveform tools

- Layer-based graphics overlay composer with broadcast frame, lower-third, information-card,
  history-timeline and blank-canvas presets. Text, shapes, lines and image layers remain editable.
- Real audio waveform generator with embedded peak envelopes, stereo lanes, colours, styles,
  progress markers and scrolling windows; source analysis has cancellable progress.
- `graphics_overlay` and `waveform_graphic` MCP tools create, inspect and edit native graphics.
  Scene/peak data survives project saves, undo and compound sequences, and renders through CPU/GPU.

## 0.2.5 (2026-10-10)

### Added

- Live tab: a **Screenshot** button saves the picture on the big display as a PNG; it appears in
  the Editor's From Live tab with the recordings, ready to drag onto the timeline (it can be mapped
  to MIDI too).
- Dither / Retro Palette goes much further: 56 palettes (computers, consoles and handhelds, colour
  depths up to 15-bit, pixel-art palettes, screens and print, and adaptive palettes picked from the
  picture) and 28 ways of dithering (Bayer 2x2 to 16x16, clustered dots, halftone, lines and
  crosshatch, blue, white and gradient noise, Knoll pattern dithering, eleven error-diffusion
  kernels and Riemersma's Hilbert curve), with Strength, perceptual (OKLab) colour matching and
  animated noise. On the GPU too, with the same result.

## 0.2.0 (2026-10-10)

Mutant Video Producer is now free software under the GNU General Public License, version 3 or later (see
[LICENSE](LICENSE)). This version is about making it hard to break: it was put through thousands of
damaged files, mangled projects and random edits, and everything that went wrong is fixed.

### Changed

- The installer puts Mutant Video Producer in Program Files for all users by default (it asks for admin rights);
  "Only for me" on its first page still installs without them. Launching Mutant Video Producer at the end of the
  installer starts it as you, not as administrator.
- Sound follows the computer's default output: plugging in headphones or connecting a Bluetooth
  device while Mutant Video Producer is open moves playback there, and unplugging the device in use no longer
  leaves it silent until a restart.
- Hardware video decoding is used for H.264, HEVC, VP9 and AV1 (where it pays); older formats such
  as MPEG-2 decode in software, where a damaged file can't upset the graphics driver.
- Crash reports list where it happened, function by function (release builds keep their symbols
  apart from the download to read them with).

### Fixed

- Rendering over a file the project uses (the source video, say) replaced it; it is now refused
  with a message.
- Undo no longer keeps a full copy of the timeline for every step: on a big project (1,700 events)
  each step took 5.4 MB, so a long session could run out of memory. A step now costs about what it
  changed (0.1 MB there).
- A copy made by Ctrl+dragging an event shared its effects' memory with the original (echo tails,
  glitch history...); it gets its own.
- Trimming or overwriting an event shorter than its fades could leave a negative fade.
- A damaged or hand-edited project file is repaired as it opens: ids used twice, events without
  length, times and sizes out of range, values that aren't numbers.
- A sound that isn't a number (from an effect pushed too far) is silenced at once instead of
  silencing the track for good.
- Detect Beats and Auto Captions stop at the end of the last audio event (an event far out on the
  timeline meant hours of work, and for captions more memory than the computer has).
- Subtitle files with absurd times (more than 99 hours) and `.cube` LUTs with `nan` or `inf` are
  read safely.
- AI control: times and colours far out of range are kept within limits.
- Files sent to a running Mutant Video Producer (a double-click on a project) are limited to a sensible size.

### Tests

- `mvp_selftest harden`: damaged and odd media, thousands of mangled project files, random
  editing with full undo and redo, export edge cases, rendering on six threads at once, memory over
  time, a big project, and mangled LUT and subtitle files.
- `MutantVideoProducer --monkey-test`: thousands of random commands, keys, clicks and drags on the editor or
  (with `--live`) the Live tab, off-screen, checking the project after every step.
- The editor test also calls every AI tool with nonsense arguments, and checks that playback
  carries on when the sound output changes.

## 0.1.5 (2026-10-09)

### Added

- A welcome window, File > New with common shapes (16:9, vertical, square...), Help on This
  (Shift+F1) and Help buttons in dialogs, a system check, and a daily check for new versions.
- A Windows installer: Start menu, `.mtg` files open in Mutant Video Producer, an uninstaller.
- A user guide, 16 tutorials and example projects in the Help menu.
- Themes: View > Theme with Classic, Neon Grid, Night City, Synthwave Sunset, Terminal, Arctic and
  Studio Light.
- Find a Command (Ctrl+K).
- One Mutant Video Producer at a time: a project opened while it runs goes to the open window.
- Crash reports: the next start offers to show it or report it on GitHub.
- A Linux build (built and tested on every push).
- Media bin, History, editable shortcuts; File > Create Clip from YouTube.
- Clicking the timeline while playing jumps there and keeps playing (or, as an option, sets where
  playback starts).

### Fixed

- Thumbnails and waveforms showed another file's after a project was reopened.
- Tooltips were hard to read on Windows.

## 0.1.4 (2026-10-08)

### Added

- Captions: Auto Captions with Whisper (on the GPU where it can), SRT and WebVTT in and out.
- Remove Background (AI, on the GPU with DirectML), freehand masks, audio-reactive effects.
- A switcher and VST3 plugins on the Live tab.
- An offline download with every speech model included.

## 0.1.3 (2026-10-08)

### Added

- Editor: autosave and crash recovery, recent projects, relinking missing media, markers and
  regions, J / K / L and I / O, auto ripple, track reordering and blend modes, slip, slide and roll
  edits, Ctrl+drag copies, looping, a grid, a crossfade switch, a Properties tab with handles on the
  preview.
- Audio: scrubbing, meters and an Audio Mixer, fade curves, auto ducking.
- Picture: Curves, Color Wheels and LUTs, freeze frames, frame export, stabilisation, effect masks
  with motion tracking, proxies, video scopes.
- Beat markers and beat montages; 16 new transitions and 13 new generators.
- Live tab: 4 x 4 effect pads (locked, momentary, in sets, switched automatically), beat detection
  with tempo locking, modulation, MIDI and MIDI clock, a replay buffer, a second layer, video files
  as a source, overlays, scenes, setlists, launching on the beat or bar, streaming, a cue monitor,
  Spout and virtual camera output, portrait and square sizes, pan and zoom, three layouts.
- AI control over MCP (Claude and other assistants can edit).

## 0.1.2 (2026-10-04)

### Added

- The Live tab records desktop audio.

## 0.1.1 (2026-10-04)

The first release.

- A non-linear editor built on FFmpeg and Qt: tracks, events, video and audio effects, transitions,
  keyframes, titles and generated media, Pan/Crop, speed and speed ramps, envelopes.
- GPU rendering (OpenGL) with hardware decoding and encoding.
- An effect chain with racks; 45 glitch, retro and psychedelic effects and 128 "broken video game
  system" effects.
- The Live tab: a camera, screen or window with effects in real time, recorded into the editor.
- About 170 render templates for platforms, devices and discs.
