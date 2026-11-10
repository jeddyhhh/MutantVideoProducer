# Changelog

What changed in each version of Mutant Video Producer. The newest is first.

## Unreleased

### Added

- **Paste Event Attributes** (Edit, Ctrl+Shift+V) gives the selected events the copied event's effects, Pan/Crop,
  fades, transitions, level, playback rate, fixes and label; **Selectively Paste Event Attributes...** picks which.
- **Replace Media** (right-click an event): it shows another file, keeping its place, trims, effects and keyframes
  (its grouped sound or picture too).
- **Smart render** (Render As): plain cuts of files already in the template's codec, size and frame rate have their
  video copied, not encoded again (MP4/MOV; cuts after the first on keyframes); otherwise it renders and says why.
- **Separate Stems** (right-click an audio event): voice and background, or vocals, drums, bass and other, each on a
  track under it, with the new **Stem Separation** AI component (Meta's Demucs).
- **De-reverb** (audio effect) takes a room's echo out of a voice, and the **Speech Enhance** preset chain does all
  the voice clean-up at once.
- AI control: paste_attributes, replace_media, separate_stems; render takes smart_render.
- **Pad sequencer** (Live tab, under the pads): a looping piano roll with a row for each pad. Click or drag to put
  blocks on it; set the bars, time signature and snap (down to 1/32 and triplets). It plays in time with the beat
  clock (tempo, taps, beats heard or MIDI clock), switching each block's pad on for its length (a momentary pad's
  block holds it on top); saved with the project.
  Ctrl+drag selects blocks to move together (onto other pads too), copy, cut, paste, duplicate (Ctrl+D) or delete;
  **⧉** pops it out into its own window, and **●** records the pads you press into the loop as it plays.
  Sequences save and load by name (the **≡** menu beside Snap).
- The **Live Set** example has a four-bar sequence ready to play (press Loop).
- Random effects chains (Create Random Effects Chain, Fill Empty Pads) now and then start with a node effect: a
  Node Editor example from Medium up, an Analogue Circuit Emu one too from High.
- The Effect Builder shows on each node whether it runs on the **GPU** (green) or the **CPU** (blue).
- The effect menus have an **Analogue Circuit Emu** menu (under Broken VGS) and a **Node Editor** menu with the
  Effect Builder's examples; **User Created** has only the effects you make. **Broken Video Game Systems** is now
  **Broken VGS**.
- Effect Builder nodes: **Feedback** (the effect's output the frame before, as a source) and **Switch** (one of four
  inputs, keyframable); Composite Signal: **PAL** (Encode and the TV), **Sync Generator**, **RC High Pass**,
  **Clipper**, **Luma/Chroma Split** and **Signal Switcher**. On the GPU the Composite Signal nodes run as shaders
  (about three times as fast: four Analogue Circuit Emu effects stacked on the Live tab in 26 ms a frame, not 83).
- **Composite Signal nodes** (Effect Builder): the picture as an NTSC composite video signal (sync, colour burst,
  subcarrier) through emulated analogue circuits onto a TV: Composite Encode, Sync Separator, Sync Mixer, RC Low
  Pass, Delay Line, RF Noise, an Enhancer (a one-transistor amplifier solved as a circuit) and Composite Decode, a
  TV that locks to the sync it's given, so damaged sync rolls and tears the picture as on a real one. 37 examples
  use them, in **Analogue Circuit Emu**: Composite Circuit wires up the whole chain; Genlock Drift,
  Multipath Ghosts, Hue Wobble, Generation Loss, Feedback Tube, Pattern Sync and the rest push it further.

### Fixed

- A Freeze in the Middle speed ramp on a very short event (a frame or two) became a slow ramp down after the project
  was saved and opened again: its jumps had been squeezed onto one time. A story chapter with no narration window
  had its footage sound's level change the same way.
- Multicam: after an angle's event was split (Split with nothing selected, Remove Silences and Insert Time split
  every track), cutting or switching to that angle past the split said "no picture there" and left a gap. An angle is
  now all the pieces of its event; a cut-short stretch's fade is kept within it. Deleting the first piece of a split
  angle (the head of a camera's file, a silence at its start) no longer loses the angle: it passes to the rest.
- Story: ripple edits on all tracks (Ctrl+Del with Ripple All, a ripple drag or trim, a paste that makes room) moved
  the clips and markers but left the chapters where they were, so a chapter was later swapped or deleted at the wrong
  place. Chapters and their regions now move with the edit; one the edit is inside of gets longer or shorter.
  A chapter cut down to nothing by Remove Silences kept its region in the old place when it was swapped or deleted.
  A compound sequence ending inside a chapter, made shorter with ripple, moved the next chapter back over it.
- Story: splitting a chapter's clip (S) left the new piece unassigned, so the chapter couldn't be swapped or deleted
  until it was assigned by hand. Both pieces are the chapter's now.
- Story: a clip dragged out of its chapter was still moved when the chapter was swapped with its neighbour (it could
  end up before the start of the timeline) and deleted with the chapter. It now stays where it was put.
- A hand-edited project's story chapter times are read within a sane range (they could overflow when chapters were
  laid end to end), and a sequence's own markers are checked as the project's are.
- Collect Project: collecting a collected project again could give two of its files the same copy (one showing the
  other's picture) when a file from elsewhere had the same name as one of its numbered copies.
- AI control: starting a render the moment the last one finished could close the program; a Render Selection to New
  Track started through AI control no longer lands in another project opened while it rendered.
- A damaged beat-grid tempo setting could close the program as it started; it now falls back to 120 BPM.
- A device dragged from one effect chain view into another (the Live tab's into the editor's, a Sequence Editor's, or
  a second copy of the program's) moved or copied whatever device sat at the same place in the chain it was dropped
  on; such drops are now refused.
- Media dragged from Project Media in a second copy of the program onto this one's timeline put down whichever of
  this project's media had the same number; such drops are now ignored.
- A screen-shader zip with an entry naming a whole path (C:/... or /...) could write that file outside the shaders
  folder; such entries are now skipped.
- The Render Queue remembered each finished render under the project open when it finished, so a queued project
  file (Add Project File...), or one queued before opening another, showed in the wrong project's Deliver History.
- Auto Captions or Edit by Transcript started through AI control put the words into whatever project was open when
  listening finished; now they go nowhere if another project was opened meanwhile.
- AI control could change the timeline while a dialog was open (Stabilize, a graphics composition, a render's
  progress), from under the event it holds: edits now wait until it is closed (reading is still allowed).
- Racks in racks are read at most 16 deep (deeper ones come in empty), so a project, preset or pad set with
  hundreds can't run the program out of stack drawing or mixing them.
- Saving an Effect Builder effect or an effect preset wrote over the old file as it went: a full disk or a failed
  write left it cut short (and said it saved). It is now written whole, then put in place.
- A node effect file (or a project carrying one) with a chain of thousands of nodes could crash the program; a graph
  now has at most 256 nodes.
- Saving a pad set or sequence whose name makes the same file name as another's ("Beat 1/2" and "Beat 1_2", or
  "Beat" and "beat" on Windows) replaced that one without asking; deleting one could delete the other.
- The Effect Builder showed the end of a long effect name and description, not their start.
- **Alternate Take**: switching a narration line to a shorter take kept fades longer than the event, and a take whose
  file has its sound in another stream played silence.
- **Smart render** copied the video of an event that was switched off (the render shows it as not there), and copied
  it too when the file had to fit a size (the messaging templates, which could then come out over their limit) or was
  asked for in another pixel format, or when the file isn't the project's shape (the render shows it with bars, the
  copy without). Those now render as usual and say why.
- **Ripple delete** of events that overlap (a crossfade) closed the overlap twice, pulling what came after too far
  back, onto the events before. Rippling all tracks left an event that started inside the deleted time where it was,
  with the next one pulled back on top of it.
- **WebVTT captions**: importing one (YouTube's, for example) showed `&amp;`, `&gt;` and the like as they are; exporting
  a caption with `<` or `&` in it (an exported .vtt or a WebM's subtitle track) wrote it bare, so players took it for
  a tag and lost the text.
- **Normalize Audio Events** did nothing on an event whose first effect was a Gain switched off.
- **Undo after Remove from Project** brought events back with no file to play (their media gone, nothing to relink):
  the file now comes back into Project Media with them. Pasting events whose file was removed does the same, and
  importing the file again gives back the same item (not a second copy).
- **Importing subtitles** broke the accents of older Windows .srt files and didn't read UTF-16 ones at all ("No
  subtitles were found").
- **A damaged project with repeated ids** lost its multicam angles when the ids were repaired on loading.
- **Split, trimmed or frozen events lost their place in an animated Pan/Crop** (a Ken Burns move, or Reframe's
  tracking): the right-hand piece started the move over. Multicam cuts started each angle's keyframes over too.
- **Compound sequences** moved a track rack's chain keyframes (a chain's level) out of step with the clips.
- **Captions with a blank line** were cut short at it in exported SRT and WebVTT files.
- **Clicks in a voice with Pitch Shift** (a cast's pitch setting puts it on the voice tracks): on real speech the
  pitch shifter held back a little more sound than it was measured to, so every few blocks a couple of samples of
  silence were played. It now keeps a tenth of a second spare.
- **Playback starting with a moment of silence**: Play now waits (the picture held on the first frame) until the
  sound is mixed, usually a fraction of a second, instead of starting the sound card on silence.
- **A gap in the sound when an event with an FFmpeg effect starts playing** (a loudness normaliser, a de-noiser, the
  pitch shift): the effect's delay was measured right then, taking up to a third of a second. Effects are measured in
  the background when the project changes, in a quarter of the time, so changing a pitch during playback doesn't
  stutter either.
- **Make Multicam** gave the Multicam track's events the same effect ids as the angles they came from (an angle
  with an effect on it then broke the project's ids).
- **Sound output** runs on its own thread, so a busy moment in the window (a repaint, a dialog) can't leave the sound
  card without sound.
- **Pasting a compound sequence** pasted only its sound: its picture (no file of its own) was taken for sound and
  left out. Copied events now remember whether they were picture or sound.
- **Closing the Effect Builder** with a node selected could crash: deleting the node told the closing window that
  the selection changed.
- **Pre-rendered frames** cleared while one was being saved, or cleared by another MVP window (they share the folder),
  were still shown done on the ruler and never made again.

## 0.9.5 (2026-11-04)

The first release built on GitHub again, for Windows and Linux. (0.9.4 was tagged, but its builds failed.)

### Fixed

- **GPU acceleration** on a computer whose OpenGL is older than 3.3 (no graphics driver, a basic display adapter)
  crashed instead of falling back to drawing on the processor.
- **Linux**: builds with the Qt 6.4 that Ubuntu 24.04 ships again (Qt's private headers, the zip reader, a list
  search), and the AppImage no longer crashes at start: the sound library makes its player when you first listen.
  (Listening in the AppImage still needs GStreamer's plugins on the system.)

## 0.9.4 (2026-11-02)

### Added

- **Live tab: Editor timeline source**: the Editor's preview as a Live source (or a layer, or a switcher input).
  Capturing plays the timeline in a loop (the time selection with Loop Playback on); stopping capture stops it.
- **Time selection bar**: right-click > Loop Playback (Play goes round the selection from the playhead inside it
  until stopped) and Clear Time Selection.
- **Multicam** takes generated pictures as angles: an animation scene filmed by several scene cameras is cut like
  cameras of the same moment.
- **AI control**: transport sets the time selection (`select`) and Loop Playback (`loop`); live_status gives the
  latest Live recording's file (`last_recording`).

### Changed

- **Menus**: File, Edit, View, Insert, Options and Help are grouped under headings, as Tools is. Options is sorted into
  Editing, Playback, Rendering and speed, Projects, Program and AI; Insert Time moved to Insert's Time group, and
  Properties next to the other project commands in File.
- The **time selection bar** above the ruler is tinted, says "Drag here to select time" while empty, and "Loop" on
  the selection while looping.

- **AI control**: get_frame draws with the GPU renderer, as the preview and renders do (screen shaders showed as if
  they weren't there). match_colour takes `auto: true` for Auto Colour.

### Fixed

- **Splitting a generated event** (a title, a credits roll, an animation scene) started its picture again in the
  second piece, and sped up the first: both pieces now carry on as the whole one did, lip sync included. Trimming its
  start cuts into it, as with a file.
- **Deliver**: compound sequences were counted as events on offline media.
- **Reopen Last Project on Start** could replace a project that AI control had opened while a startup question (such
  as Recover Work) was still on the screen.
- **Retro Shaders** came out as "Unknown (retro.glsl-shaders-master/...)": Get Screen Shaders unpacked GitHub's folder one
  level too deep (Qt lists the folder's own entry without its slash). Shaders already downloaded that way are found too.
- **Track Corners** followed the shot ending under the event, not the one going on, when the two overlapped at the
  cursor by a moment; and when it can't follow, it says how far each corner got.

- A new install starts with the **Synthwave Sunset** theme and a neon green seven-segment timecode.

### Fixed

- **Live tab**: the setup's scroll bar covered the controls at its right edge (Mirror, Reset...).

### Added

- **J and L cuts**: Alt+drag an edge of a grouped event trims its picture or its sound alone; Ctrl+Alt+Shift+drag a cut
  rolls that track's cut alone. They stay grouped.
- **Ctrl+D** (Edit > Duplicate): a copy of the selected events right after them, starting where they end.
- **Retro Shaders**: RetroArch's screen shaders (libretro's GLSL presets: CRT, handheld LCD, NTSC, VHS, pixel-art
  upscalers; over 600) as video effects on the GPU renderer, their parameters as knobs. Tools > Get Screen Shaders
  downloads them (each under its own licence); "Picture fed in" sets the lines they see (240 to start).
- **Pre-rendering**: while stopped, the heavy parts near the cursor (slower to render than a frame lasts) are rendered in
  the background and the preview plays them from there; green under the ruler when done, orange until then. Edits make
  their own stretch new. Options turn it off and clear its frames.
- **Colour labels** on events (right-click > Colour Label; eight colours; AI control: edit_event label).
- **Spectrogram** of an audio event (right-click, or Tools), as heard or before its effects; a click moves the cursor.
- **De-hum** (notches at 50 / 60 Hz and harmonics) and **De-click** (clicks, pops, crackle; optionally clipping) audio
  effects.
- **Timeline**: faint lines down the tracks at the ruler's marks while the snap grid is off (Options > Grid turns them
  off).
- **Guides over the preview** (View > Guides, or the Guides button): title and action safe areas, the rule of thirds,
  the centre, and 9:16, 4:5 and 1:1 crops. Never rendered.
- **Auto Colour** (Tools, or right-click a video event): black and white points and white balance from the picture, as
  a new **White Balance and Auto Colour** effect, which also has **Auto** and **Pick Neutral** (click something grey or
  white on the preview).
- **Corner Pin** effect (the picture's corners anywhere, in perspective, on the GPU too) and **Track Corners**: its
  corners follow a screen or sign in the shot under the event. AI control: track_motion on a corner_pin.

## 0.9.3 (2026-11-01)

### Changed

- **AI control**: a tool called with an argument it doesn't take is refused, naming the ones it takes. It was ignored,
  so a misspelt or invented argument could quietly do something else than meant.

- **Effect Builder effects run on the GPU**: node by node on the graphics card instead of the CPU. The examples at 720p
  take 3 to 14 ms a frame instead of 45 to 170 ms, so they keep up on the Live tab (and play faster in the editor).
- **Live tab**: the Live FX, the active pad and held pads go through the GPU in one go (one upload, one read back),
  instead of once each.
- **Animated backgrounds and overlays draw on the GPU**: Plasma and Aurora as shaders (12 and 18 ms a 1080p frame down
  to 3 and 4), Starfield / Hyperspace (58 to 8), Retro Grid (31 to 6), Bokeh, Digital Rain, Confetti, Countdown, Progress
  Bar, Letterbox, Shape, Kinetic Text and Color Bars on the GPU's painter.
- **Reduce Noise and Optical Flow** work out the motion between frames on the GPU (no frames read back): Reduce Noise
  15 ms a 1080p frame down to 6.
- **Live tab**: the layer (fitted, with its own effects, blended) and the transition when switching pads run on the GPU
  in the same single trip as the Live FX and pads.
- **Effect Builder**: its preview runs on the GPU (each node's picture too), so heavy graphs play smoothly while you build.

### Fixed

- **Live tab**: animated sources inside a user effect (plasma, starfield, titles...) stood still there.
- Effects on the CPU renderer could crash or tear on the Live tab: the picture they changed was shared with the capture.
- **Generate Dialogue** with a long script froze the program for many minutes once the voices were done: each line's
  file rebuilt the media lists and the profile library again. Importing many files at once had the same cost.
- **Auto Captions**: a caption could stay on screen through a long pause (the music turned down under the voice was
  heard as speech, and a caption could reach into the next line); after a long silence captions could drift seconds
  early; silences came out as "[BLANK_AUDIO]" captions.
- **Beat Montage** takes each file's clips from its Mark In / Out (Project Media), not from anywhere in the file.
- **Animation scenes**: a path drawn on with trim_end lost its fill even once the whole stroke showed.

### Added

- **AI control**: remove_markers takes the markers out of a stretch of time (the beat markers once a montage is cut).
- **Live tab**: four layers instead of one, each with its own source, settings and Layer FX (pick it in the Layer
  section's Layer list; Layer 1-4 FX in the pads' Edit list). A **Size** slider (5-100%) for picture in picture, and
  **No Effects**: the layer is laid over after the Live FX and pads, which leave it as it is. Drag a picture-in-picture
  layer on the big Live picture to put it anywhere.
- **Status bar**: CPU and GPU load, RAM, the GPU's temperature and memory (NVIDIA), once a second.
- **Help > About**: links to jeddyh.fyi and the YouTube channel.
- **Device chain**: pictures of the settings beside the controls, most of them dragged to change them: the Parametric
  EQ's and the Filter's curves, the Compressor's, Noise Gate's and Limiter's transfer curves, the Delay's echoes, the
  Reverb's tail, the Chorus's and Flanger's sweep, the Stereo Tools' field, the colour curves of LUT colour effects,
  the hues before and after of colour-matrix effects, and a centre or position on the picture. Short choices are a row
  of buttons. Knobs show their settings' whole names (wider, then on two or three lines).
- **Device chain > Add**: the Preset Chains, as in the FX tabs. A chain saved with Save Chain is there straight away.
- Effect Builder effects are in the nested sequence editor's Add FX list and in the graphics editor's effect on a layer,
  as soon as they are saved.

## 0.9.2 (2026-10-31)

### Added

- **Effect Builder**: opened from the Live tab, the example is what is live (the big Live picture, before its effects;
  the colour bars when nothing is live). 14 more example patterns to try effects on (SMPTE bars, gradients, a
  checkerboard, plasma, aurora, starfield, hyperspace, retro grid, digital rain, bokeh, a countdown).

### Changed

- **Program icon**: the play symbol in the middle is gone.

### Fixed

- **Live tab**: double-clicking a user effect in a device chain opens it in the Effect Builder (it did nothing).

## 0.9.0 (2026-10-30)

### Added

- **Effect Builder** (Tools > Effect Builder (Node Graph)): make your own video effects by wiring nodes, with the example
  video on the left, the graph in the middle (each node showing its picture) and the result live on the right. 249
  nodes: every video effect, the backgrounds, patterns and text as sources, and nodes to blend, mix, do maths, mask,
  key, split and combine channels, threshold, displace, place and delay. Settings ticked "Show on effect" become the
  effect's (keyframable). Saved, the effect is in the Effects tab under User Created; projects carry its graph.
- AI control: `node_effect` builds, previews and saves Effect Builder graphs, and reads saved ones back.
- AI control covers the rest of the menus: `envelope` (track volume / pan / composite automation and custom speed
  ramps), `group`, `duplicate_events`, `stabilize`, `track_motion`, `beat_montage`, `media` (relink, remove, organize,
  proxies), `presets`, `normalize_audio` and `youtube_clip`; `edit_track` sets pan and the compositing mode, and
  `get_frame` can save the frame to a file.
- Eleven example node effects come with the program (User Created > Examples; Tools > Edit User Effect > Examples
  opens them to see how they're made).
- A user effect's box in the device chain says to double-click it: that opens it in the Effect Builder, and saving
  there updates that event's effect.

- **Track mattes**: a video track can be a matte for the track below it (the header's Mode box, or right-click >
  Track Matte): not shown itself, it decides where that track shows, by where it is drawn (alpha: video inside a
  title's letters) or where it is bright (luma), or the other way round. The same on the CPU and the GPU.
- **Match Colour** (Tools > Match Colour to the Cursor Frame, or right-click an event): gives the selected events the
  colours of the shot under the cursor, as a Color Match effect (red, green and blue curves found by matching the
  two pictures' levels) with an Amount.
- **Reduce Noise** (right-click a video event): Light, Medium or Strong noise reduction for grainy footage, from the
  pixels around each one in its frame and the frames either side, followed along the motion; edges stay sharp.
- AI control: `edit_track` `matte`, `edit_event` `denoise`, and `match_colour`.

## 0.8.5 (2026-10-29)

### Added

- The timeline's timecode fills its corner; click it for another font (7-segment and dot-matrix displays among
  them), right-click it for its colour (neon green, amber, LED red...). Both are remembered.

- **Adjustment layers** (Insert > Adjustment Layer): an event with no picture whose effects change everything below
  it while it lasts, with fades and opacity; the same on the CPU and the GPU.
- **Tools > Render Selection to New Track**: renders the time selection to a ProRes file on a Rendered track on top,
  so heavy stretches play smoothly. Both compositors now start at the topmost track whose picture covers the whole
  frame (an opaque event from a file, no fades, effects or pan/crop), skipping what it hides, which speeds up plain
  stacked footage too.
- **File > Export Stems**: each audio track with sound to its own 24-bit WAV, after its effects, level, pan and
  ducking, before Master FX.
- AI control: `render` takes `in_place` (render from-to onto a Rendered track) and `stems` (a WAV per audio track
  into the folder `path`).

- **Tools > Sync Selected Events by Audio**: lines up a second camera or a separate sound recorder with the first by
  their sound (to the millisecond), and leaves alone a recording that isn't of the same moment.
- **Multicam**: Tools > Multicam > Make Multicam from Selected Events puts the cameras under a Multicam track; press
  1-9 while the edit plays (or click an angle in the Show Angles window) to cut between them, or, stopped, to switch the
  stretch under the cursor.
- AI control: a **multicam** tool (sync by audio, make a multicam, cut and switch angles), and `edit_event` sets
  `frame_blend` and `optical_flow`.
- **Make in-between frames (optical flow)** in Event Properties: slow motion that follows the motion between two frames
  and draws the frames in between, on the CPU and the GPU.

- Live: **Xbox controllers** (XInput on Windows, joystick devices on Linux) are in the MIDI list as Xbox controller 1-4.
  Buttons and the D-pad map to pads, scenes and the other Learn MIDI actions; the triggers and each stick direction
  drive effect settings through Modulate > MIDI, or press an action when pushed past half way.

- Timeline: drag up or down on the time ruler to zoom (around the time you grabbed; a sideways drag still scrubs), and
  drag empty track space to move the timeline left and right. The selection box is now **Ctrl+drag** (it adds to the
  selection); **Shift+drag** still makes a time selection.

- AI control: a `user_guide` tool reads the User Guide and tutorials, and a new page,
  [Making a whole video through AI control](docs/ai-workflow.md), gives an assistant the workflow for an animated
  episode or documentary (sound first, a scene per shot, titles, checks, finishing). The server's instructions point
  to it, so a newly connected assistant starts from the workflow instead of trial and error.

### Changed

- AI control: `edit_event` with `speed` now changes the event's length with it (half speed, twice as long), as its
  description always said; it used to keep the length and show less of the file. `keep_length: true` keeps it.

- The Deliver tab's choices (outputs, ticks, folder, loudness target, and the publish pack's title, description and
  tags) are saved in the project file instead of this computer's settings, so they go with the project. Older projects
  bring theirs in the first time Deliver opens them.

## 0.8.0 (2026-10-25)

### Changed

- Faster renders of animation episodes (Four Score at 320x240: 30 s of picture in 16 s, was 39 s): the audio mix runs on
  its own thread beside the pictures, audio effects pass silent stretches by (a voice track's pitch shift and echo cost
  nothing between its lines), and scenes are drawn with less work (their layer tree built once and kept, poses worked out
  once a moment, only a layer's changing settings evaluated, no readback for a soft layer without effects of its own).
- Animation scenes on the GPU: motion blur, soft layers (blur and depth of field), a layer's own effects and groups seen
  through as one no longer draw CPU pictures when rendering with GPU compositing, and the preview paints scenes on the
  GPU instead of drawing ahead on the CPU. An episode scene with motion blur: 235 ms a frame to 68 ms (export), 80 ms to
  35 ms (preview).

### Added

- **Tools > Record Voice-Over...**: records a microphone while the edit plays from the cursor, and puts each take on a
  **Voice-over** audio track where it started (one undo step; an overlapping take gets its own track). Takes are WAV
  files beside the project.
- **Master volume** on the Editor: a white line across the transport bar's meter. Drag it to turn the whole mix down
  (all the way left is silence), double-click it for 0 dB. It comes after Master FX, is saved with the project, goes into
  exports and can be undone.
- Animation scenes: **fixed** layers ignore the camera (**Ignores the camera** in the composer, `"fixed": true`), so lyrics,
  captions and logos stay put through close-ups, pans and the speaker camera. On a group it holds all its children.
- Character library: **Terry the towel**, a striped bath towel with sleepy eyes (his towel and stripe colours are
  settings).
- **Animation**, a clip type of its own (**Insert > Animation**, the Generators' Animation category): characters, sets,
  props, moves, cameras and the rest of the animation engine, edited in **Compose Animation...**. Graphics Overlay keeps
  to text and graphics, and its composer shows only those tools. Overlays holding an animation in earlier projects open
  as Animation clips. AI control: `graphics_overlay` works on both; a motion preset or version 2 scene makes an Animation.
- The Tools menu is grouped under headings: Editing, Music and beats, Voices and captions, Sequences, Finishing and
  publishing, Proxies and AI.
- Compose Graphics, **Add prop...**: a library of props (phone, megaphone, shovel, lunchbox, mitten, clapperboard, mug,
  sign) held in a character's hand, moving with the arm. AI control: `graphics_overlay` `props`, `add_prop`.
- Moves: walk in, walk out, float, fall, get up, sit and stand.
- Timeline: a scene's expression keys show on its event, and drag along it.
- Speaker camera shots: a reaction of the listener, a two-shot or the wide shot for a while (`camera.speaker.shots`).
- Animation scenes: a scene's `time` plays it backwards, holds it (a freeze) or slows it; a video layer's `source_time`
  picks the moment of its file; `noise` layers (TV static, film grain, fog) and `flash` layers (at times, or on the beat).
- Freeze Frame in place (AI control: `freeze_frame` `in_place`): the held frame covers the following time and nothing
  moves later.
- Sets: their signs are scene variables (`set_banner`, `set_chalk`, `set_menu`...), reworded in the Keys tab.
- AI control, `graphics_overlay` `dance`: characters move on the beat of the music under a scene, in one step.
- Animation scenes: a text layer can show the line being said on a bound voice track (`{"subtitle": binding}`).
- AI control, `edit_event` `loop`: a file repeats so its event can run past its end; a length the file can't fill comes
  back with a warning instead of being shortened silently.
- Generate Dialogue scripts: `(pause 2)` leaves silence before the next line, and `[radio]`, `[phone]`, `[megaphone]`,
  `[echo]`, `[hall]`, `[dream]` or `[backwards]` at the start of a speech gives it that sound.
- AI control, `graphics_overlay` `patch`: change scene properties by path (a sign's words, a layer's effects, the camera)
  without sending the whole scene; and `batch`: many moves, expressions, keys and patches as one undo step.
- Animation scenes hold up to 1024 layers (was 512): about a dozen library characters and a set.
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
- **Options > Audio Sync with Picture...**: for outputs that add their own delay (TV, HDMI or monitor speakers,
  Bluetooth headphones), how long the picture waits for the sound.
- Compose Graphics, **Add character...** and **Save as character...**: a library of cut-out characters (kids, a man, a
  teacher, a woman, a bear) that comes with every installation, plus your own; named, recoloured with scene variables,
  bound to a voice by their name. AI control: `graphics_overlay` `characters`, `add_character`, `save_character`.
- **Mouth shapes**: a character's mouth takes the shape of the words (A, E, O, L, F, M, rest), from each line's text or
  aligned word times; any layer can use `{audio, viseme}`.
- **Lip sync follows edits**: a binding made from a voice track is analysed again whenever the track's lines (or the
  scene) move, in the same undo step.
- Keys tab, **Moves**: hop, jump, shake, nod, wave, spin, walk, pop, squash and blink at the playhead. AI control:
  `graphics_overlay` `move`.
- The look: layer **shading**, **contact shadow**, **blur** and **depth** (with the camera's **depth of field**), and
  scene **motion blur** (Scene tab > Look).
- Compose Graphics, **video** layers: a video file playing inside a layer's box (a TV, a phone, a window), cover or
  contain, with its start, speed and looping.
- Compose Graphics, **Add set...**: a library of 17 ready-made backgrounds (some with a front part drawn in front of the
  characters), one per scene, plus your own. AI control: `graphics_overlay` `sets`, `add_set`.
- Compose Graphics, **Effect on this layer...**: any video effect on one layer or group of a scene only.
- **Camera frames whoever is talking** (Scene tab > Look): the camera cuts to the loudest character by itself.
- **Expressions** (Keys tab): library characters look neutral, angry, sad, surprised or smug from the playhead on; and
  the gestures **point**, **arms up** and **shrug** in Moves. AI control: `graphics_overlay` `expression`.
- Generate Dialogue, **Exact mouth timing**: each word of each line is timed (with a Whisper model) so mouth shapes land
  on the words. AI control: `generate_dialogue` `align`.
- Playing a heavy scene, the preview draws motion blur and layer effects at draft quality; renders are unchanged.

### Fixed

- A move on a character (pop, squash, the dance's squashes) repeated with its breathing: the move's keys went into the
  breathing loop. A move now stops the loop on what it moves. A move on a property the layer didn't have also no longer
  starts from an empty key.
- Closing the program could hang (the window gone, the program still running) after animation scenes with video layers
  had played: their decoders were freed as render threads ended, where FFmpeg's threads can't finish.
- A two-shot or reaction shot from the speaker camera could zoom in and out for a few seconds: the two people it framed
  were chosen again on every frame (the listener swapping between people who had spoken, the speaker dropping out at a
  pause). They are now chosen once for the whole shot.
- Library characters held their arms behind their bodies, hiding their hands and the props in them. Their arms are now in
  front of the body (behind the head).
- Deliver showed another project's title, description and tags: the choices made while a project was unsaved went to
  every project saved after it. They now go only to that project, on its first save.
- The speaker camera cut to the wide shot for a moment at a short pause in a line, or between two lines; it now
  holds the close-up through pauses of up to 2.4 seconds (the speaker camera's `hold`).
- The speaker camera could flick between two people for a frame or two: at a hand-over (it held on whoever was loudest,
  and the end of one line and the start of the next were as loud: it now holds on whoever was heard last), and as a
  reaction shot ended (it went back to the speaker who had just finished). A framing that would last less than half a
  second now gives way to the one after it (the camera's `glimpse`).
- The Limiter let loud high-frequency material (clicks, cymbals, noise) swing past its ceiling between samples (a -3 dB
  ceiling measured -0.9 dBTP); its true-peak estimate now matches the loudness meter's.
- The Limiter caught sample peaks only, so a mix limited at -1.5 dB still measured above -1.5 dBTP; it now holds its ceiling as a true peak (between samples, oversampled four times).
- Lip sync ignored reversed lines: a line played backwards kept the mouth shut and the speaker camera away. Its sound and mouth shapes now play backwards too.
- Audio finishing told a mix already at its loudness target that it needed a limiter for more gain.
- Animation scenes: text animated a word at a time lost the spaces between its words at small sizes ("DIRECTOROF"),
  and centred text sat slightly off centre. Text is now drawn exactly where its layout puts it.
- Playing an animation scene with characters was slow (a few frames a second at full preview quality): every hidden
  mouth shape and expression was still drawn off screen, every outlined shape built an outline nobody used, a whole
  frame was blurred for one small soft layer, and the scene was checked again on every frame. A scene with four
  characters now draws three to five times faster, a soft-lit one ten times faster, and while playing the preview
  draws a scene's next frames ahead on the other cores, with lighter motion blur (renders keep it in full).
- Playing, the timeline redrew every event on every frame just to move the playhead; it now redraws the playhead.
- A layer's effects could paint outside the layer: a CRT on a TV in a scene put black corners on the whole picture,
  and noise covered the whole frame. They now work on the layer's own box.
- With scene motion blur, the speaker camera showed a double picture on the frame where it cut to the next speaker;
  every moment of a frame now frames the same speaker.
- A layer's motion blur drew its overlapping parts added together (too bright where they overlap); its moments are now
  averaged.
- Playing in the editor, the sound could fall behind the picture for the rest of the playback (lip sync looked
  early) whenever the mix started late or hiccupped, as it does the first time an effect such as Pitch Shift is used
  and its delay measured: the silence played meanwhile was counted as played time. The picture now follows the sound
  actually handed to the speakers. Exports were always in sync.
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

### Timeline wheel: zoom over the ruler, scroll elsewhere

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
