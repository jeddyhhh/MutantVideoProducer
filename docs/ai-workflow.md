# Making a whole video through AI control

This page is for an AI assistant connected through [AI Control (MCP)](ai-control.md); read it with the `user_guide`
tool (`page: ai-workflow.md`). It is the way the *Snowbank* cartoon episodes (ten minutes each: characters, voices,
lip sync, sets, camera cuts, titles, captions, mixed sound) were made, step by step, with the tools for each step and
the checks that catch mistakes. The same order works for a documentary, an explainer or a music video: leave out the
steps you don't need.

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

## Limits worth knowing

- A scene holds up to 1024 stored layers: a set and about a dozen library characters.
- Colours are `#RRGGBB`, or `#AARRGGBB` with the alpha **first**; use `opacity` for see-through.
- A clip on a TV that is shorter than the shot runs out: give the `video` layer `loop: true`, or check the end of the
  shot in `get_frame`.
- `graphics_overlay` `capabilities` lists the layer types and limits; `characters`, `sets` and `props` list the
  libraries.

See also [Animation Scenes](animation-scenes.md), [Captions and Narration](captions.md), [Audio](audio.md),
[Deliver](deliver.md) and the [animated short tutorial](tutorials/17-animated-short.md).
