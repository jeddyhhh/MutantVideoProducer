# Making a whole video through AI control

This page is for an AI assistant connected through [AI Control (MCP)](ai-control.md); read it with the `user_guide`
tool (`page: ai-workflow.md`). It is the way the *Snowbank* cartoon episodes (ten minutes each: characters, voices,
lip sync, sets, camera cuts, titles, captions, mixed sound) were made, step by step, with the tools for each step and
the checks that catch mistakes. The same order works for a documentary, an explainer or a music video: leave out the
steps you don't need. To edit real footage instead (interviews, a vlog, a talk, a music video), see
[Editing real footage](#editing-real-footage) below.

## How a call looks

Every tool takes one JSON object. Ids (`event_id`, `media_id`, track indexes) come from `get_project`; times are
seconds. Some examples, in the order a cartoon uses them:

```json
{"tool": "generate_dialogue", "arguments": {
  "script": "Pip: Is it snowing?\n(pause 1.5)\nMoss: [radio] Base to Pip, come in.",
  "cast": {"Pip": {"voice": "af_heart", "rate": 1.1, "pitch": 3},
           "Moss": {"voice": "<another list_voices id>", "rate": 0.9, "pitch": -4}},
  "start": 2, "gap": 0.3}}

{"tool": "graphics_overlay", "arguments": {
  "action": "create", "scene": {"version": 2, "layers": []}, "at": 0, "length": 24.5, "track": 0}}

{"tool": "graphics_overlay", "arguments": {
  "action": "add_character", "event_id": 41, "character": "<a name from action characters>",
  "name": "Pip", "x": 0.2, "colours": {"jacket": "#d7263d"}}}

{"tool": "graphics_overlay", "arguments": {
  "action": "bind_audio", "event_id": 41, "source_track": 3, "binding": "pip"}}

{"tool": "graphics_overlay", "arguments": {"action": "batch", "event_id": 41, "operations": [
  {"action": "expression", "layer_id": "pip", "mood": "surprised", "time": 1.8},
  {"action": "move", "layer_id": "pip", "move": "hop", "time": 2.1},
  {"action": "patch", "operations": [{"layer_id": "$camera", "property": "speaker", "value": {"zoom": 1.7}}]}]}}

{"tool": "get_frame", "arguments": {"time": 12.0, "width": 960}}
```

The layer id of a character is the one `add_character` returns (and `get` lists); use that, not a guess. A call
that fails changes nothing and says why in its text: read it, fix the arguments, and call again.

## Before you start

- `get_project` shows the media, tracks, events and markers; ids for every other tool come from it.
- `new_project` (`width`, `height`, `fps`, `discard: true` only when the user agreed) starts empty;
  `save_project` with a `path` early, then again after each stage.
- Everything is undoable (`undo`), and the user sees each change in the editor as it happens.
- Look at what you made: `get_frame` at a time returns the picture. Check often; it is the only way to see a scene
  you built from numbers.
- Times are seconds on the timeline, except inside an animation scene, where keys and moves use **event-local**
  seconds (0 = the start of that event).
- Long jobs (voices, captions, renders) run in the background: poll their status, and don't edit or switch projects
  while a voice job runs.
- Leave the final render to the user unless they ask for it.

## 1. Plan on paper first

Write the script and a shot list before touching the timeline:

- **Shots**: a name, the set (`graphics_overlay` `sets` lists them), who is in it and where they stand, the lines, and
  what happens (moves, moods, sound effects, a flash, a clip on a TV). A ten-minute episode was 20 to 25 shots and
  about 130 lines.
- **Cast**: per character a library character (`graphics_overlay` `characters`), colours, and a voice
  (`list_voices`) with a `rate` and `pitch`, so each sounds different.
- **Pacing**: lines are spoken back to back, so plan the silences: a lead-in before a shot's first line, pauses for a
  reaction or a sound effect, a tail after the last.

## 2. Sound first

The picture is cut to the sound, so make all the speech before any scene.

1. `generate_dialogue` with the whole script (one `Name: line` per line) and the `cast`. In the script:
   - a line `(pause 2)` adds two seconds of silence before the next line (lead-ins, beats, room for a sound effect);
   - `[radio]`, `[phone]`, `[megaphone]`, `[echo]`, `[hall]`, `[dream]` or `[backwards]` at the start of a line gives it
     that sound.
2. Poll `voice_status` (its `dialogue` field) until it finishes. Each character gets an audio track named after them.
3. `get_project` again: each line is an event on its character's track, in script order. Their start and end times
   are the timing of the whole film: work out each shot's start and end from its first and last line plus the lead-in
   and tail you planned.
4. Music and sound effects: `list_sound_effects` (a CC0 library), `import_media`, `add_media_to_timeline` at a time and
   track, then `edit_event` for `gain`, `fade_in` and `fade_out`. Put music on its own track and duck it under the
   voices: `edit_track` `duck_under` (the voice track) and `duck_amount`.
5. A character's voice effect for the whole film: `add_effect` with `target: track` on their track.
6. Beats, for cutting or dancing on the music: `detect_beats` puts markers on them.

## 3. One animation event per shot

Make a video track named `Animation` (`add_track`, `edit_track` `name`) above any clip tracks. For each shot:

1. `graphics_overlay` `create` with `scene: {"version": 2, "layers": []}`, `at` the shot's start and `length` its
   length, on that track. `edit_event` `name` it after the shot (the timeline, chapters and checks read better).
2. `add_set` (one per scene; a new one replaces it).
3. `add_character` for each person: `character` (library name), `name` (who they are: their **binding** is the name in
   lower case, and must match the voice track's binding below), `x` (left edge, 0 to 1), `ground` (feet, 0.9 by default)
   and `colours`. Space three or four people across the frame; more than about a dozen characters won't fit in a scene.
4. **Lip sync**: `bind_audio` with `source_track` (the character's voice track index) and `binding` (their lower-case
   name), for everyone who speaks in the shot. The mouth follows every word, and still does after the voice track is
   edited.
5. **The speaker camera**: `patch` `$camera` property `speaker` with `{"zoom": 1.7}` cuts to whoever is talking, holds
   through short pauses and goes wide when nobody talks. `camera.speaker.shots` adds reaction shots, two-shots and wide
   shots for chosen times (see [Animation Scenes](animation-scenes.md)).
6. **Acting**, at event-local times from the line times: `expression` (`mood`: neutral, angry, sad, surprised, smug)
   just before a line, `move` (hop, jump, shake, nod, wave, spin, walk with `to`, pop, squash, point, arms up, shrug)
   on a beat of the line, `add_prop` to put something in a hand, `dance` on the music's beats. Send many of them in one
   `batch` (one round trip, one undo step).
7. **More**: a clip on a TV is a `video` layer (`path`, `source_in`, `fit`), `flash` layers for lightning or camera
   flashes, `noise` layers for snow, fog, film grain or TV static, a scene `time` key for slow motion or a freeze, and
   `add_effect` with `target: event` for a look on the whole shot (vignette, grain, colour).
8. Text that must stay on screen through close-ups (subtitles, lyrics, a score, a logo) needs `"fixed": true`, or the
   camera moves it with the scene.

Small changes go through `patch` (a property by its dotted path) instead of `get` and `update` of a whole scene.

## 4. Titles, subtitles and lower thirds

Put them on a video track named `Titles` above `Animation`, as Graphics Overlays:

- `graphics_overlay` `presets` lists the ready layouts (`lower_third`, title cards, broadcast layouts); `create` with
  the preset's scene and your words.
- Subtitles for one character: a text layer whose `text` is `{"subtitle": "<binding>"}` shows the line playing on
  that voice, after `bind_audio` of that voice track onto the overlay.
- `waveform_graphic` draws a line's real waveform (a radio message, a phone call).
- Opening titles and credits can be animation events of their own (a roll is a text layer keyed upwards).

## 5. Check before finishing

- `get_frame` the middle of every shot and a moment either side of each cut. Look for black or empty frames,
  people standing off the edge, faces behind props or the set's front, text cut off or pushed off screen, and a camera
  framing nobody.
- Play through the busy parts of the edit (`transport`) when timing matters: a move should land on its line or beat.
- `get_project` for gaps: a shot that ends before the next one starts shows black.
- Fix with `patch`, `batch` or `edit_event`, then look again.

## 6. Finish

1. **Chapters**: `add_marker` with a `length` makes a region; one per act, named, gives YouTube chapters.
2. **Captions**: `captions` `action: auto` transcribes the speech onto a captions track; mute that track if you don't
   want them burnt in (`edit_track` `muted`) and `captions` `action: export` an SRT for the upload.
3. **Loudness**: `add_effect` on `target: audio_master` (a compressor, then `a_limiter`), then `audio_finishing`
   `apply_gain` to `-16` LUFS with a `-1.5` dBTP ceiling, and `analyze` to confirm.
4. **Deliver**: `deliver` `outputs` (a pack such as YouTube, and ticks), `pack` (title, description with `{chapters}`,
   tags, `thumbnail_time`), and `check` until it is ready.
5. `save_project`. Render (`render`, or `deliver` `render`) only when the user asks.

## Editing real footage

Interviews, a vlog, a talk, a documentary or a music video start from files, not a script.

1. **Bring it in.** `import_media` (`paths`), then `get_project` for the media ids. `add_media_to_timeline`
   (`media_id`, `at`, `track`) places each file; its sound comes grouped with it. A long take can be cut down later,
   so place it whole.
2. **Several cameras.** Put each camera's file on its own track, then `multicam` `action: sync` (`event_ids`) lines
   them up by their sound, and `action: create` makes one multicam event. Cut between angles with `action: cut`
   (`angle` 1-9, `time`: that angle from then on) or `action: switch` (the stretch under `time` shows `angle`);
   `action: angles` lists the angles and cuts.
3. **Take out the dead air.** `cut_time` `action: silences` (`from`, `to`, `threshold_db` -40, `min_length` 0.7,
   `keep` 0.15) removes pauses on every unlocked track, keeping everything in sync. For a talk:
   - `cut_time` `action: transcribe` (`model`: tiny, base or small) returns the words with their times and changes
     nothing;
   - pick the ums, false starts and retakes from that list;
   - `cut_time` `action: words` with the same `words` and `remove` (their indexes) cuts them, with the pause after each run.
   Check a cut or two with `get_frame` and `transport` play: a jump cut in mid-sentence is a word removed too many.
4. **Shots inside a file.** A downloaded or already-edited video has its own cuts: `scene_changes` (`event_ids`,
   `sensitivity` 0.5) splits it there, so each shot can be moved or dropped. `split` (`time`, `event_ids`) cuts by
   hand; `delete_events` with `ripple: true` closes the gap.
5. **Fix the picture.** `stabilize` (`event_id`, `smoothing`, `zoom`) for shaky handheld shots; `match_colour`
   (`event_ids`, `time`: a frame whose colours the others should take), or `auto: true` to correct each shot on its
   own; `edit_event` `denoise` (0..1) for dark or phone footage.
6. **Music.** Music on its own audio track, `edit_track` `duck_under` the voice track so it drops while people talk.
   For a music video, `detect_beats` then `beat_montage` (`media_ids`, `every` 1-16 markers) cuts the footage to the
   beat on a new track, or cut by hand on the markers.
7. **Structure.** For a documentary, `story_chapters` plans chapters with a narration window and listening time
   (`action: add` with `name`, `script`, `narration`, `listening`), and `generate_voice` speaks each chapter's
   narration into it. Otherwise regions from `add_marker` with a `length` are the chapters.
8. **Words on screen.** `add_text` with `preset: "lower_third"`, `text`, `at` and `length` for names; `captions`
   `action: auto` for subtitles, with `style: kinetic` for words that pop on in time with the speech (good on a short).
9. **Vertical versions.** `auto_reframe` (`event_ids`, `portrait: true`) makes a 1080x1920 version that follows the
   subject; save it under another name with `save_project` `path` first, so the wide edit stays.

Then finish as in step 6 above: chapters, captions, loudness, Deliver.

## Effects, keyframes and masks

1. **Find the effect and its settings.** `list_effects` with `kind` (video, audio, transition, generator) and a
   `search` returns the type ids and every setting's id, range and choices. Use those ids exactly; don't guess them.
2. **Add it.** `add_effect` with `target` (`event`, `track`, `output`, `audio_master`, `live`, `pad` or `layer`),
   `id` (the event id, track index, pad 1-16 or layer 1-4), `effect` and `params` (only the settings that differ from
   the defaults). It returns the `effect_id`. A setting's value is a number, `true` / `false`, a choice's name, a
   `#rrggbb` colour or text.
3. **Keyframe it.** Any number takes `{"keyframes": [[seconds, value, curve], ...]}` instead of a value. On an event's
   effect the seconds count from the event's start. Curves: `linear`, `hold`, `smooth`, `ease_in`, `ease_out`,
   `ease_in_out`, `bounce`, `elastic`, or `bezier` with `[x1, y1, x2, y2]`.
4. **Mask it.** Video effects take `mask.shape` (`Off`, `Rectangle`, `Ellipse`, `Freehand`), `mask.x` and `mask.y`
   (its centre, 0..1 across the picture), `mask.w` and `mask.h` (its size, 0..1), `mask.feather` (0-30, a percentage
   of the picture's height) and `mask.invert` (the effect everywhere but the mask). A freehand mask has `mask.points` (3-16)
   and `mask.p<i>x` / `mask.p<i>y` for each point.
5. **Make the mask follow.** `track_motion` (`event_id`, `effect_id`, `from`) keyframes the mask's position on every
   frame to the end of the event. The event needs a steady forward speed (no reverse or speed ramp).
6. **Change or remove it.** `set_effect` (`effect_id`, `params`, `enabled`) and `remove_effect` (`effect_id`).
   `get_project` lists every effect with its id and settings.
7. **Reuse a look.** `presets` `action: list` (with `search`, `audio`), then `action: apply` onto a chain (`target`,
   `id`) or onto one effect (`effect_id`); `action: save` keeps an effect or a whole chain under a `name`.

Blurring a face that moves, then fading a vignette in over the first two seconds of the same event:

```json
{"tool": "add_effect", "arguments": {"target": "event", "id": 17, "effect": "gaussian_blur",
  "params": {"horizontal": 30, "vertical": 30, "mask.shape": "Ellipse",
             "mask.x": 0.5, "mask.y": 0.3, "mask.w": 0.16, "mask.h": 0.24, "mask.feather": 6}}}

{"tool": "track_motion", "arguments": {"event_id": 17, "effect_id": 233, "from": 4.0}}

{"tool": "add_effect", "arguments": {"target": "event", "id": 17, "effect": "vignette",
  "params": {"amount": {"keyframes": [[0, 0, "ease_in_out"], [2, 0.6]]}}}}
```

`233` stands for the `effect_id` the first call returned; `from` is timeline seconds. Check with `get_frame`
before and after: a mask in the wrong place blurs the wrong thing for the whole shot.

## Cleaning up sound

Listen first, then fix, then measure:

1. **Look at the problem.** Right-click > Spectrogram is the user's view; for you, `get_project` shows the audio
   events and `audio_finishing` `action: analyze` reports levels, peaks and silences for the whole mix.
2. **Repair one event.** `add_effect` with `target: event` on the audio event:
   - `a_denoise` (Noise Reduction: `amount` 1-60 dB, `floor`) for steady hiss, fans or air conditioning;
   - `a_dehum` (`mains`: `50 Hz` or `60 Hz`, `harmonics` 1-8) for mains hum;
   - `a_declick` (`threshold`: lower finds more) for clicks and crackle;
   - `a_dereverb` for a voice in an echoey room;
   - `a_gate` to silence the noise between phrases, `a_eq` and `a_compressor` to shape and even out a voice.
   Small steps: too much noise reduction makes a voice sound underwater.
3. **Split a mix.** `separate_stems` `action: start` (`event_id`, `stems` 2 for voice and background, 4 for vocals,
   drums, bass and other, `best` for the slower model). Poll `action: status`; each stem lands on its own track under
   the event, which is muted. Then turn the music down under speech, or take the voice out of a song.
4. **Even the levels.** `normalize_audio` (`event_ids`) brings each event's loudest peak to -0.1 dBFS, so clips
   recorded at different levels start alike. `edit_event` `gain` then sets the balance.
5. **Finish the mix.** Effects on `target: audio_master` (`a_compressor`, then `a_limiter`), then `audio_finishing`
   `action: apply_gain` (`lufs` -16, `true_peak` -1.5) and `action: analyze` to confirm.

## A clip from YouTube

Only for videos the user may use: their own, Creative Commons, or with permission. Ask if it isn't clear.

1. `youtube_clip` (`url`, `from`, `to`, `quality` 1080 / 720 / 480 / 360, or 0 for the sound only, `place`, `at`)
   downloads just the part asked for, imports it and places it. It takes as long as the download. If YouTube asks to
   verify, `browser` (`firefox`, `chrome`, `edge` or `brave`) uses that browser's session.
2. Trim it (`edit_event`, `split`, `delete_events`), cut at its own shot changes (`scene_changes`) or take out pauses
   (`cut_time` `action: silences`).
3. `captions` `action: auto` for subtitles; `add_text` for a title or a credit to the source.
4. For the upload: `youtube_publish` `action: chapters` (from story chapters or markers, with warnings where YouTube
   wouldn't show them), `action: track_listing` (`track`: an audio track's events as a numbered list) and
   `action: thumbnail` (`time`, `title`, `path`: a 1280x720 picture). Put them into `deliver` `pack`.

## Preparing a Live set

The Live tab plays effects over a camera, screen or video in real time. You can set everything up; performing and
going live are the user's.

1. `live_status` reads it all: the sources to choose from, the pads, scenes, setlist, tempo and what is recording.
2. **Sources.** `set_live_source` (`video`, `audio`: part of a name from `live_status`). For more cameras,
   `live_switcher` `inputs` sets inputs 2-5 by name.
3. **Pads.** `set_pad` (`pad` 1-16, `name`, `effects`: `[{"type": id, "params": {...}}, ...]`, `momentary` for pads
   that are on only while held: strobes, freezes, glitch bursts). Locked pads are left alone. The chain types are the
   same ids as `list_effects`; `add_effect` with `target: pad` adds one effect to a pad's chain, and `target: live` to
   the Live FX on top of everything.
4. **Tempo.** `set_tempo` (`bpm`, `lock`) for a known tempo; leave `lock: false` to let the beat detection settle on
   the music.
5. **Scenes.** Get a look right (Live FX, a pad, the layer, overlays), then `save_scene` (`scene` 1-8, `name`).
6. **Try it.** `live_action` `action: start`, then `pad` / `scene` with a `number`, or `cue_pad` / `cue_scene` and
   `take` to preview before it goes on air; `get_live_frame` shows the output. `action: stop` when done.
7. **Record.** `live_action` `action: record` / `stop_recording` (and `save_replay`); `live_status` `last_recording`
   is the file, which can go straight onto the timeline with `import_media`.

There is no tool for going live on a streaming site, and the stream key is never shown: leave that to the user.

## Several versions of one edit

- Save each version as its own project (`save_project` with a new `path`), such as the 16:9 upload and a vertical cut.
- `render_queue` `action: add` queues the open project as it is now (`path`, `preset`, `from`, `to`), or another
  version without opening it (`project_file`). `action: start` works through them; `action: status` shows progress.
- `deliver` `outputs` with a pack (`youtube`, `social`, `masters`...) ticks a set of outputs for one project, and
  `deliver` `render` renders them all through the same queue.
- As always, start renders only when the user asks.

## When a tool says no

The reason is in the reply. The common ones:

- **A window is open in Mutant Video Producer** (a dialog, a progress window, a sequence editor): tools that change
  the project wait until it is closed. Ask the user to close it; tools that only read still work.
- **A locked track**: edits that would change it are refused. `edit_track` `locked: false` unlocks it, if the user
  locked it for a reason, ask first.
- **Unsaved changes**: `open_project` and `new_project` refuse unless `discard: true`. Save first, or ask.
- **A voice job is running**: let `voice_status` say it has finished before editing or switching projects.
- **Out of range**: numbers are checked (a time past the end, a speed of 0, a track index that doesn't exist).
  `get_project` again: ids and indexes change when tracks are added or removed.
- **Nothing changed**: the call was valid but had nothing to do (no silences under the threshold, events of the
  wrong kind). Check the arguments against `get_project`.

## Limits worth knowing

- A scene holds up to 1024 stored layers: a set and about a dozen library characters.
- Colours are `#RRGGBB`, or `#AARRGGBB` with the alpha **first**; use `opacity` for see-through.
- A clip on a TV that is shorter than the shot runs out: give the `video` layer `loop: true`, or check the end of the
  shot in `get_frame`.
- `graphics_overlay` `capabilities` lists the layer types and limits; `characters`, `sets` and `props` list the
  libraries.

See also [AI Control](ai-control.md) for every tool, [Animation Scenes](animation-scenes.md),
[Captions and Narration](captions.md), [Story](story.md), [Audio](audio.md), [Deliver](deliver.md), the
[AI assistant tutorial](tutorials/16-ai-assistant.md) and the [animated short tutorial](tutorials/17-animated-short.md).
