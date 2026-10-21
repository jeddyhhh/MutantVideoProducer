# Changelog

What changed in each version of Mutant Video Producer. The newest is first.

## Unreleased

### Added

- **Insert > Insert Time at the Cursor...**: empty time on every unlocked track, the opposite of Cut Time; what follows,
  its markers and its story chapters move later, and a region or chapter across it grows. AI control: `cut_time`
  `insert` with `at` and `length`.
- Compose Graphics, Keys tab, **Follow audio**: any property of a layer follows a bound sound (loudness, either
  channel or the beats) with its silent value, how much is added at full volume, attack and release, and **Stop
  following**; a mouth that moves with a voice track no longer needs JSON.
- **Animation: characters** (Generators, Graphics): two cut-out characters whose mouths, arms and bodies follow the
  bindings `voice_a` and `voice_b`, who blink, and whose colours are scene variables.
- **Tools > Captions > Generate Dialogue from a Script...**: `Name: line` lines spoken with a voice, speed and pitch
  per character, each character on their own track named after them, lines in script order, one undo step; voices
  are remembered per name. AI control: `generate_dialogue`.
- **Sounds** tab: a built-in library of 67 short sound effects in every installation (transitions, hits and booms,
  comedy stings, notifications, everyday sounds), searchable, to preview, drag onto the timeline or add at the
  cursor. All CC0, credited in `sfx/CREDITS.md`. AI control: `list_sound_effects`.

### Fixed

- The editor stopped responding while many files were added at once (a long generated dialogue, then an import) with
  a large profile media library on a slow drive: every file added looked up each library entry on disk, rewrote the
  library, and rebuilt Profile Media, and the profile home folder's watcher (any autosave or voice take) rebuilt it
  again. Library paths are now compared without the disk, each file is looked up once, and only the library file is
  watched, with the editor's own writes ignored.
- Compose Graphics crashed when a starting layout (or an imported template) with fewer layers replaced a scene whose
  last layers were selected: the old layer list was read in the new, shorter scene before it was rebuilt. (#7)
- Compose Graphics looks up layers by their row in one place, which skips a row the scene no longer has (grouping,
  making a reusable object, deleting, setting a key and the canvas did not check); the Go Live dialog's service list
  likewise.

- Installing an AI engine on a drive without room said only "not enough disk space"; it now names the drive, how
  much it needs and has, the folder, that a CUDA already installed doesn't reduce it, and how to install elsewhere
  (**Folder...**, below).
- AI Components shows the engines folder and its free space, and **Folder...** puts the engines on another drive
  (the default, in your app data on C:, can be too small for the 6-12 GB that setup needs); **Default** goes back.

### Testing

- The monkey test uses every dialog it opens (dropdowns, list rows, numbers, text, buttons, canvases) before
  answering it, and opens editors such as Compose Graphics on purpose; three seeds run in the test suite.
- The test suites also run on a Debug build, whose Qt checks every index: a read past the end of a list stops with
  the line instead of passing unnoticed.

## 0.7.0 (2026-10-19)

### Added

- Lip sync from a voice track: a graphics scene can bind its animation to every sound on a track during the scene (each
  character's voice track, under the character's name), so a mouth moves through all of that character's lines. In
  Compose Graphics, Bind audio for animation lists the audio tracks and takes a binding name; for AI control,
  `graphics_overlay` `bind_audio` takes `source_track`.

- Two new tutorials: **An Animated Short with Lip Sync** (voice tracks per character, building characters, lip sync,
  blinking, camera cuts) and **Delivering a Finished Edit** (the Deliver tab end to end).
- The guides now cover crash reports, Director's Dice and the Deliver tab in the README, the timecode format, freeze
  frames on generated pictures, speaker captions, limiter-aware safe gain and the newer AI control settings; the user
  guide's index links Animation Scenes, Graphics through AI Control and GPU Acceleration.

### Fixed (found by the edge-hunt projects)

- Renders that a short range made impossible: a fit-to-size template (Discord, WhatsApp...) over a few seconds asked
  the encoder for gigabits a second, and NVENC refused it; the bitrate now stays under about a bit per pixel.
- 4:2:2, 4:4:4 and RGB templates (H.264 all-intra, editing masters) stay on the software encoder: NVENC's H.264
  failed with "cannot encode pixel format yuv422p".
- A render or a saved project into a folder that doesn't exist yet makes the folder.
- AI control:
  - `edit_event` gives an event's speed, trim and reverse to its grouped sound too, and `split` cuts the sound
    with its picture (they went out of step); starts and lengths are on whole frames (at 29.97 fps the gaps of a
    fraction of a frame between placed events rendered as black flashes); `source_in` stays inside the file
    (past the end it left a zero-length event).
  - Edits that would change a locked track are refused, as the timeline refuses them.
  - Refused: an FFmpeg audio filter chain FFmpeg can't use (it passed the sound through unchanged, silently), an
    unknown Director's Dice style or track type, a frame rate outside 1-240 (0 fps was accepted);
    `cut_time` ranges stop at the end of the edit (removed time was counted past it).
- The FFmpeg Audio Filter's chain box turns red with an explanation when FFmpeg can't use the chain.
- AI control: `add_text` and `edit_event` take `params`, the generator's own settings (a title's font, size and colour, a
  starfield's speed, a plasma's palette), which only the window could change before.

### Fixed (found by the generated-only projects)

- Deliver: outputs, folder and details chosen before a project's first save were lost when it was saved, and its
  files kept the name "Untitled"; they now move to the saved project and take its name.
- Motion graphics: a chart naming a dataset the scene doesn't have, or with more than 128 rows, is refused with a
  reason (it drew nothing, or dropped the rows past 128, silently).
- Auto captions with speaker turns gave each turn as one caption (up to 140 characters on one line, off both sides
  of the picture); turns are now cut on words into captions of the chosen length, each timed by its share.
- Freeze frame on a generated picture said there was no video event there; it now says generated pictures can't be
  frozen.
- Deliver: after the render queue was stopped, the tab kept saying "Rendering 10 of 12" with a time left that grew
  for ever; it now says "Stopped: 9 of 12 rendered", and outputs rendered before don't count again.
- Timecode (burn-in, the timeline, file names) wrote ";" before the frames at every frame rate, which reads as
  drop-frame; whole rates such as 25 and 50 fps now use ":", and ";" stays for 29.97 and 59.94.
- AI control: `add_track` with no index put every new track at the bottom, under the sound; a video or image track now
  goes on top, as its description says. The graphics tool and docs now say that 8-digit colours are #AARRGGBB (alpha
  first), so `#00000088` is clear rather than half-dark.
- Audio finishing: Apply safe gain put its gain after a Limiter in Master FX, so the limiter could never catch the peaks
  and a quiet mix with loud peaks could not be brought up to the target (it even turned it down). The gain now goes in
  front of the limiter and reaches the loudness target (up to +12 dB); the warning says to add a limiter.
- A crash could leave no crash report: a library loaded later could replace the crash handler. It is put back every
  few seconds, and the report names the library that replaced it.

## 0.6.0 (2026-10-18)

### Deliver tab

- A third tab beside Editor and Live gets a finished edit out. A **header** shows how ready the edit
  is and renders every output with one button, with overall progress and time left. A **strip** shows
  the whole edit: frames, chapters, markers and stretches with nothing on screen; click a frame for
  the thumbnail. **Checks**: offline media, gaps, loudness against a chosen target on a meter,
  YouTube chapters, platform length limits (Trim) and captions, each with a button to go there or fix
  it. **Outputs**: templates or one-click packs, each with its size, codecs, estimated file size and
  progress; the total size against the free space. **Publish pack**: title, thumbnail, tags and a
  description with `{chapters}` / `{tracklist}` placeholders and YouTube's character counts, saved
  with the captions in one go. **History**: every render of the project, from anywhere, with its
  length checked and Render Again. See [Deliver](docs/deliver.md).
- AI control: the `deliver` tool reads the checks and readiness, sets up the outputs (templates,
  packs, ticks, trims, folder), renders them, writes the publish pack and reads the history.

### Live: the size stays the size

- On the Live tab, 1080p, 720p, 540p and 360p are fixed 16:9 outputs: a window source of another
  shape no longer changes the frame to its proportions. **Framing** (crop to fill, fit with bars,
  fit on a blurred background) fits it in. **Source size** still follows the source.

### Caption and graphics colours

- On the timeline, captions and text events are gold and graphics overlays and waveforms are violet, in every
  theme. A track holding only captions or only graphics shows that colour in its header too.

### Track lock

- Right-click a track header > **Lock Track** protects its events. They can't be selected, moved,
  trimmed or dropped onto, Split leaves them whole, and ripples leave them in place. The header
  shows a padlock, the events are hatched, and the lock is saved with the project.

### YouTube chapters and thumbnails

- **Tools > YouTube > Chapters for the Description...**: `00:00 Name` lines from the story
  chapters or the markers, with an *Intro* at 00:00 when needed, and a note when YouTube wouldn't
  show them (fewer than 3, or any under 10 s).
- **Tools > YouTube > Thumbnail from the Cursor Frame...**: a 1280x720 JPEG of the frame at the
  cursor with an optional outlined title, kept under 2 MB.

### Split at scene changes

- **Tools > Split Selected Events at Scene Changes...** finds the cuts between shots (colour
  histograms of every frame, compared with the shot's own motion) and splits the selected video
  events and their grouped sound there, with a sensitivity setting.

### Remove silences, edit by transcript

- **Tools > Remove Silences...** cuts the quiet stretches (threshold, minimum length and padding
  are adjustable) out of every unlocked track at once and closes the gaps.
- **Tools > Edit by Transcript...** transcribes the edit word by word with Whisper; strike words in
  the text and they are cut from the timeline, with the pause after them.
- Both work on the time selection or the whole timeline, keep markers in step, and undo in one step.

### Render queue

- **File > Render Queue...**: renders run one after another in the background, each of the project
  as it was when added. **Render As > Add to Render Queue** queues the open project (in any
  template, again and again); **Add Project File...** queues other projects without opening them.

### Auto reframe for vertical

- **Tools > Auto Reframe for Vertical...** crops wide video for 9:16 (offering to switch the
  project to 1080x1920): the subject is found four times a second (a person through the Remove
  Background model, else the movement) and becomes a smooth Pan/Crop path that holds still,
  follows calmly and stays inside the picture.

### Fixed (found by test projects)

- Titles, credits and kinetic text are sized for the widest 16:9 area of the frame, so they no longer run off
  the sides of 4:3 and vertical (9:16) projects.
- 10-bit templates on a GPU encoder (NVENC, Quick Sync, AMF) use P010; HEVC 10-bit with hardware encoding
  failed with "cannot encode pixel format yuv420p10le". H.264 10-bit stays in software.
- Auto Reframe gives reversed and speed-ramped events a still crop centred on the subject instead of
  leaving them letterboxed in a vertical edit.
- The compound-sequence refusal names the event that overlaps the selection (often a crossfade at its edge).
- AI control: freehand mask points (`mask.p0x`...) can be set, a choice can be named without its explanation
  ("Off" for "Off (everywhere)"), `render` / `render_queue` refuse a path whose extension the template doesn't
  write, `add_text` says when a preset has no text, and `new_project` keeps NTSC rates (23.976 was 23.952).
- AI control, from the noughties test projects: a still picture asked onto a video track goes there (it went
  onto a new Image track at the top, moving every track index); `add_text` and `graphics_overlay` return the
  track they used, with a note when a new track was added for them, and `add_text` checks for room with its
  real length; a template is matched to the output's extension first ("MP3" for a .mp3 was the DivX AVI);
  `render` warns when the render is longer than the template's platform allows (as the Render dialog does).
- The Graphics Overlay Composer's canvas takes the project's shape. It was always 16:9, so in 4:3 and vertical
  projects pictures and text previewed at the wrong size.

## 0.5.0 (2026-10-16)

### Timeline wheel works like Ableton Live

- The mouse wheel zooms only over the time ruler, where the pointer becomes a magnifying glass.
  Anywhere else on the timeline the wheel scrolls the tracks up and down. Ctrl+wheel or
  Shift+wheel scrolls sideways.

### Music uploads to YouTube

- **Audio-Only YouTube (album art, 5 fps)** render template: 1080p H.264 tuned for a still
  picture at 5 fps, with AAC 384 kb/s.
- Right-click an audio track's header > **Track Listing for YouTube...** lists its events as
  `01. Name - 04:19` with names from the file names, editable before copying.

### From YouTube shows this project's downloads

- The **From YouTube** tab lists only the current project's downloads. **Show downloads from
  other projects** lists the rest of the profile's downloads, now with thumbnails (also in
  Profile Media).

## 0.2.7 (2026-10-16)

### Keyframe Editor

- A **Keyframes** tab shows every keyframe of an event (Event FX with racks, Pan/Crop, generated
  media, transitions), a track, the output or the master. The **Dope Sheet** picks keyframes by
  click or box, then moves, copies, stretches, reverses, nudges, copies and pastes them, also into
  another event. The **Graph** drags values and Bezier handles on the real curves.
- New keyframe curves: Ease In, Ease Out, Ease In/Out, Bezier (with handles), Bounce and Elastic,
  also on volume, pan and speed envelopes (no Bezier there). Smooth handles fit Bezier curves
  without overshoot. Projects stay readable by older builds, which show the new curves as Smooth.
- Controls' right-click menus offer every curve and **Edit in Keyframe Editor...**. MCP keyframes
  take `[seconds, value, curve, [x1, y1, x2, y2]]`.
- `mvp_keyframes_test` covers the curves, their file format, speed-ramp spans and every edit.
  `--devices-test` drives the editor.

### Linux AppImage and milestone offline builds

- Linux builds are also packaged as an AppImage (Qt and FFmpeg inside), smoke-tested in CI and
  attached to version releases with the tar.gz.
- The Windows offline edition is built only for milestones: run the release workflow manually
  on the version tag with "offline" ticked. Tag pushes no longer build the frozen AI engines.
- The build guide matches the release builds (Qt 6.8.3, Visual Studio 2022+, FFmpeg and AI
  helper scripts).
- `mvp_adversarial_test` writes to the temp folder by default instead of the current folder;
  `mvp_story_test` says what it needs when started without a video.

### Faster speech on NVIDIA GPUs without RVC

- The CUDA speech engine installs its own cuDNN 9.7, cuBLAS, CUDA runtime and cuFFT (hash-locked),
  so GPU narration no longer requires the 12 GB CUDA RVC engine; it needs 6 GB free to install.
  Measured: 37.7 s of narration in 7.9 s including start-up, with no CUDA Toolkit on the system.
- The voice worker prefers those libraries and falls back to CUDA RVC's.

### Fixed

- Resizing a story chapter keeps every chapter on its own clips. At 24 fps, chapter ends drifted
  1 µs off the frame grid, so later edits failed with "Chapter ranges must not overlap". Projects
  saved with the drift are healed the next time a chapter is resized.

## 0.2.6 (2026-10-14)

### Fixed

- The preview no longer crashes when its GPU renderer is recreated (for example after switching
  GPU Acceleration or Hardware Video Decoding) while titles or other drawn graphics are on the
  timeline. The OpenGL context is now released in a way Qt's per-thread paint engine can follow.

### Tests

- The `gpu` suite recreates the GPU renderer on one thread and checks that a title still draws.
- The monkey test answers menus again with Qt 6.8, where off-screen popups never become the
  active popup; menus opened from a step no longer hang the run.
- The captions check matches its documented fixture ("Welcome to Mutant Video Producer...").

## 0.2.5 (2026-10-10)

### Small Windows installer and optional AI engines

- Standard Windows installer/ZIP excludes the large speech/RVC frameworks and bundled
  model caches, with a hard size budget below 1 GiB. Native editing, rendering, captions
  and background-removal backends remain in the base distribution.
- Tools > AI Components downloads speech and RVC dependencies directly from Astral,
  PyPI and PyTorch. CPU/CUDA choices, verified pinned wheels, visible setup output,
  cancellation and repair are built in. No separate Python installation is required.
- One normal Windows setup EXE embeds the editor payload with disk spanning disabled.
- Windows setup defaults to Mutant Video Producer for its installation and Start menu
  folders, ignoring saved Montage folder names from earlier installations.
- Per-user isolated environments are shared across profiles and projects. Atomic
  activation preserves working engines on failed repairs; bundled full/offline engines
  remain supported. Existing generated media plays without installed AI engines.
- MCP ai_components exposes the same status/install flow. Tests cover invalid catalogs,
  modified interpreters, escaped active pointers, failed repair preservation, lean
  packaging without frozen workers, and real upstream speech generation.

### Cross-platform builds and media paths

- Fixed the graphics composer's Qt 6.4 string/JSON comparison that blocked Linux builds.
- Relative file imports retain absolute paths, so saving into another folder keeps media
  online. Regression checks cover image rendering, recovery and exported audio automation.
- Blank/audio-reactive generator fixtures now test their intended behaviour; headless CI
  explicitly skips unavailable GPU checks while keeping the CPU and composer assertions.
- Windows Actions builds main and publishes an installer/ZIP; Linux Actions publishes an
  Ubuntu 24.04 archive after tests. Packaged Windows AI workers are cached between builds.
- Windows FFmpeg downloads select a completed dated 8.1 build and verify its SHA-256,
  avoiding transient 404s while the publisher replaces the moving latest release.
- Bundled workers are deployed only when an executable for the target platform exists.
- Branch build downloads are stored in draft releases with SHA-256 checksums; large files
  have verified multipart reconstruction. Delivery no longer requires Actions artifact quota.
- Windows CI builds the complete app and its executed tests with bounded parallelism,
  avoiding redundant whole-program links of suites already run on Linux.

### Audio finishing and graphics workflow

- Audio Finishing measures the complete post-master mix: integrated, momentary and
  short-term loudness, loudness range, true peaks, stereo balance/correlation and silence.
  Editable targets, mix warnings, JSON reports and an undoable peak-limited master gain
  are available from Tools, the mixer and MCP.
- The graphics composer adds snapping guides, multi-layer moves, alignment/equal-gap
  distribution, keyboard nudging, saved layer/group locks and reusable font/colour brand kits.
  Layout preserves animation and supports recording keys. MCP exposes layout, lock and brand actions.
- Added regression coverage for intersample peaks, silent/invalid audio, actual mixer
  normalization, transformed/animated layouts, locks and brand styles.

### New project format and icon

- Projects now use `.mvp` and a Mutant Video Producer format identifier with a mandatory
  version. Old `.mtg` projects are unsupported, with no import or conversion path.
- Updated project dialogs, autosaves, collection, MCP, examples, tutorials and installer
  associations to the new format. Unsupported files are omitted from recent-project menus.
- Added a cyberpunk neon green and blue icon to the executable, installer and app windows.

### Product rename

- Renamed the application to Mutant Video Producer, including Windows metadata,
  installer, MCP identity, documentation, tutorials and example narration.
- Source, support and update links now point to `jeddyhhh/MutantVideoProducer`.
- Presets, profile libraries, preferences and default media/model folders are retained;
  isolated tests verify the settings migration. The project format is now superseded above.
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
- A Windows installer: Start menu, `.mvp` files open in Mutant Video Producer, an uninstaller.
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
