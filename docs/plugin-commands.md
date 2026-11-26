# Plugin Commands

Every command a plugin's script can run with `mvp.call(name, arguments)`; they are also the commands
[AI Control](ai-control.md) gives an assistant. Times are in seconds; ids (`event_id`, `media_id`,
`effect_id`) come from `get_project`; track indexes count from 0 at the top. Each command answers with an
object, and throws an `Error` with the reason when it can't do what it was asked. See [Plugins](plugins.md).

This page is made from the program by `scripts/plugin_commands_doc.py`; in a script, `mvp.tools()` gives the
same list.

## Project and media

### get_project

The whole project: settings, media (ids, files, length, size), tracks with their events (event ids, where they sit, what they show, their effects with ids and settings), track and output effects, markers, the cursor, and the events selected on the timeline (selected: true on each, and the selected list).

No arguments.

### new_project

*Not available to plugins (a plugin works on the open project, in one undo step).*

Starts a new, empty project (File > New). Refuses while there are unsaved changes unless discard is true.

| Argument | Type | What it is |
|---|---|---|
| `discard` | boolean | Drop unsaved changes |
| `fps` | number | Frame rate (default 30) |
| `height` | integer | Picture height (default 1080) |
| `width` | integer | Picture width (default 1920) |

### open_project

*Not available to plugins (a plugin works on the open project, in one undo step).*

Opens a .mvp project (File > Open). Refuses while there are unsaved changes unless discard is true. Lists files it can't find.

| Argument | Type | What it is |
|---|---|---|
| `discard` | boolean | Drop unsaved changes |
| `path` (required) | string | The .mvp file |

### save_project

Saves the project (to its file, or to a new .mvp path).

| Argument | Type | What it is |
|---|---|---|
| `path` | string | A .mvp file (default: the project's own file) |

### collect_project

Creates a portable project and media folder from the current edit, with progress and a missing-file/dependency report. Does not change the open project.

| Argument | Type | What it is |
|---|---|---|
| `destination` (required) | string | New folder inside an existing parent; existing folders are never overwritten |
| `include_unused_media` | boolean | Include unused Project Media (default true) |
| `include_voice_models` | boolean | Include saved RVC models (default true) |

### set_project_settings

The project's picture size and frame rate.

| Argument | Type | What it is |
|---|---|---|
| `fps` | number | Frames per second (23.976, 25, 29.97, 30, 50, 59.94, 60...) |
| `height` | integer | Pixels |
| `width` | integer | Pixels |

### import_media

Adds files (video, audio, pictures) to Project Media. Returns their media ids.

| Argument | Type | What it is |
|---|---|---|
| `paths` (required) | list of string | Full file paths |

### media

Project Media: relink a missing (or moved) file, remove a file nothing uses, organize one (folder, rating, Mark In / Out: the part placed from it), or make proxies (smaller copies that play smoothly; in the background).

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `relink` / `remove` / `organize` / `proxies` | What |
| `folder` | string | organize: e.g. Footage/Day 1 |
| `mark_in` | number | organize: seconds |
| `mark_out` | number | organize: seconds (0: to the end) |
| `media_id` | integer | relink / remove / organize: the file (see get_project) |
| `media_ids` | list of integer | proxies: the files (default: every heavy one) |
| `path` | string | relink: the file where it is now |
| `rating` | integer | organize: 0..5 stars |
| `replace` | boolean | proxies: make them again |

### replace_media

Right-click > Replace Media: event_ids (and their grouped sound or picture) show media_id instead, keeping their place, trims (where the new file is long enough), effects and keyframes. One undo step.

| Argument | Type | What it is |
|---|---|---|
| `event_ids` (required) | list of integer | Events |
| `media_id` (required) | integer | The new file (import_media first) |

### youtube_clip

File > Create Clip from YouTube: downloads a video (or a part of it) with yt-dlp into the YouTube folder, imports it and (unless place is false) puts it on the timeline. Only for videos you may use (your own, Creative Commons, or with permission). Takes as long as the download.

| Argument | Type | What it is |
|---|---|---|
| `at` | number | Seconds: where (default: the cursor) |
| `browser` | string | Use this browser's session if YouTube asks to verify: firefox, chrome, edge or brave |
| `from` | number | Seconds: the part's start (default: the start) |
| `place` | boolean | Put it on the timeline (default true) |
| `quality` | integer | Up to 1080, 720, 480 or 360 lines; 0: the sound only (default 1080) |
| `to` | number | Seconds: the part's end (default: the end) |
| `url` (required) | string | The video's link |

### user_guide

Mutant Video Producer's User Guide and tutorials (Markdown), the same pages as Help > User Guide. Without page: the list of pages with their titles. Read ai-workflow.md first to make a whole video (an animated episode, a documentary, a music video): the order of the work, the tools for each step and the checks.

| Argument | Type | What it is |
|---|---|---|
| `page` | string | A page from the list, e.g. ai-workflow.md, animation-scenes.md, tutorials/17-animated-short.md |

## Placing and editing events

### add_media_to_timeline

Places a media file on the timeline at a time: its picture on a video track and its sound on an audio track, grouped. Tracks are created when missing. Returns the new event ids.

| Argument | Type | What it is |
|---|---|---|
| `at` | number | Timeline time in seconds (default: the cursor) |
| `media_id` (required) | integer | From get_project / import_media |
| `track` | integer | Track index to prefer (0 = top); default: automatic |

### add_text

Adds a text / title event (or another generated picture: see list_effects kind=generator).

| Argument | Type | What it is |
|---|---|---|
| `at` | number | Start, seconds (default: the cursor) |
| `length` | number | Seconds (default 5) |
| `params` | object | The generator's settings (list_effects kind=generator details=true): font, size, color, speed, palette... as add_effect params, keyframes included |
| `preset` | string | Generator preset id (default "title"; e.g. lower_third, credits, solid_black) |
| `text` | string | The words |
| `track` | integer | Video track index (default: the first) |

### edit_event

Changes an event: where it starts, how long it is, where in its file it starts, its track, name, mute, opacity / volume (gain), speed, fades. Its group moves with it and takes the same speed, trim and reverse.

| Argument | Type | What it is |
|---|---|---|
| `denoise` | number | Video: noise reduction 0 (off) .. 1 (strong; 0.35 light, 0.65 medium), for grainy dark or phone footage (slower to preview) |
| `event_id` (required) | integer | Event (clip) id |
| `fade_in` | number | Fade in, seconds |
| `fade_out` | number | Fade out, seconds |
| `frame_blend` | boolean | Video slow motion: blend neighbouring frames |
| `gain` | number | Opacity (video) or volume (audio), 0..1 (audio up to 4) |
| `keep_length` | boolean | With speed: keep the event's length and show more or less of the file instead |
| `label` | string | Colour label on the timeline: red, orange, yellow, green, teal, blue, purple, pink or none |
| `length` | number | Seconds |
| `loop` | boolean | Repeat the file so the event can run on past its end (false: it stops there; a length past the end without loop is shortened, with a warning) |
| `muted` | boolean | Mute |
| `name` | string | Label |
| `optical_flow` | boolean | Video slow motion: make in-between frames by following the motion (the smoothest; slower to preview and render) |
| `pan_crop` | object | Event Pan/Crop settings (mode Fit/Fill/Stretch, centerX, centerY, zoom, rotation, flipH, flipV), values or {keyframes: [[seconds from the event's start, value, curve?], ...]} |
| `params` | object | A generated event's settings (as add_text params) |
| `ramp` | `speed_up` / `slow_down` / `slow_middle` / `fast_middle` / `freeze_middle` / `none` | A speed ramp preset (its grouped sound follows); none removes the ramp |
| `reverse` | boolean | Play the source span backwards |
| `source_in` | number | Where in the media it starts, seconds |
| `speed` | number | Playback rate 0.05..20 (the length follows: half speed, twice as long) |
| `start` | number | Timeline position, seconds |
| `track` | integer | Move to this track index (same kind: video / audio / image) |
| `transition_in` | object | Video: the transition at its start (its fade in, or the crossfade with the event it overlaps): {type: a transition id from list_effects kind=transition, params: {setting: value}} |
| `transition_out` | object | Video: the transition at its end, as transition_in |

### split

Cuts events at a time: the ones given (with their group), or every event under that time.

| Argument | Type | What it is |
|---|---|---|
| `event_ids` | list of integer | Events to cut (default: all under the time) |
| `time` (required) | number | Seconds |

### delete_events

Removes events (with their group).

| Argument | Type | What it is |
|---|---|---|
| `event_ids` (required) | list of integer | Events |
| `ripple` | boolean | Close the gap (later events on those tracks move up) |

### duplicate_events

Copies events (with their effects, fades and groups) to a new time on the same tracks (Edit > Copy, Paste at the cursor). Returns the new event ids.

| Argument | Type | What it is |
|---|---|---|
| `at` | number | Seconds: where the earliest of them goes (default: the cursor) |
| `event_ids` (required) | list of integer | Events |

### group

Groups events (they move, trim and fade together; Edit > Group) or ungroups them (e.g. to delete a video event's sound without its picture).

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `group` / `ungroup` | What |
| `event_ids` (required) | list of integer | Events |

### paste_attributes

Edit > Selectively Paste Event Attributes: event from's effects (replacing theirs), Pan/Crop, fades... onto event_ids of the same kind (picture or sound). attributes: effects, pan_crop, fades, transitions, level, speed (each keeps its length), fixes (stabilize, noise reduction, slow-motion blending), label (default all). One undo step.

| Argument | Type | What it is |
|---|---|---|
| `attributes` | list of string | Which (default all) |
| `event_ids` (required) | list of integer | Events to paste onto |
| `from` (required) | integer | The event to copy from |

### freeze_frame

Holds a video event's picture: at the time, the event is split and its frame there is held for the length (later events on that track move along; with in_place, the hold covers the time after it instead and nothing moves). For an animation scene, key its "time" instead (graphics_overlay).

| Argument | Type | What it is |
|---|---|---|
| `in_place` | boolean | Hold over the following time, moving nothing later (its sound plays on) |
| `length` | number | Seconds held (default 2) |
| `time` (required) | number | Seconds, over a video event |

### cut_time

Takes time out of the whole edit, on every unlocked track at once, closing the gaps (markers follow), or puts it in (insert: length seconds of empty time at `at`, what follows moving later): silences (Tools > Remove Silences), ranges (seconds), transcribe (Edit by Transcript's words with their times; changes nothing) or words (cut the words given by index from a transcribe result, with the pause after each run).

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `silences` / `ranges` / `transcribe` / `words` / `insert` | What |
| `at` | number | insert: seconds (default: the cursor) |
| `from` | number | silences / transcribe: seconds (default 0) |
| `keep` | number | silences: seconds left either side (default 0.15) |
| `length` | number | insert: seconds of empty time |
| `min_length` | number | silences: seconds (default 0.7) |
| `model` | `tiny` / `base` / `small` | transcribe: speech model (default base) |
| `ranges` | list of values | ranges: [[from, to], ...] in seconds |
| `remove` | list of integer | words: indexes of the words to cut |
| `threshold_db` | number | silences: quieter than this is silence (default -40) |
| `to` | number | silences / transcribe: seconds (default: the end) |
| `words` | list of values | words: the words from transcribe ([{start, end, text}, ...]) |

### add_track

Adds a video, audio or image track.

| Argument | Type | What it is |
|---|---|---|
| `index` | integer | Where (default: video on top, audio at the bottom) |
| `type` (required) | `video` / `audio` / `image` | Kind |

### edit_track

Changes a track: lock it (its events can't be edited, ripples pass it by), mute, solo, rename, set its level, or make it a track matte.

| Argument | Type | What it is |
|---|---|---|
| `blend` | string | Video: compositing mode over the tracks below: Normal, Add, Screen, Multiply, Overlay, Lighten, Darken or Difference |
| `duck_amount` | number | Audio: how far, dB (-60..0, default -12) |
| `duck_under` | integer | Audio: turn this track down while that track (index) has sound; -1 stops |
| `level` | number | Video: opacity 0..1; audio: volume 0..2 (1 = 0 dB) |
| `locked` | boolean | Lock or unlock |
| `matte` | `off` / `alpha` / `alpha_inverted` / `luma` / `luma_inverted` | Video: make this a track matte for the visual track below it (not shown itself; that track shows where this one is drawn, alpha, or bright, luma), or off |
| `muted` | boolean | Mute |
| `name` | string | New name |
| `pan` | number | Audio: -1 (left) .. 1 (right) |
| `solo` | boolean | Solo |
| `track` (required) | integer | Track index (see get_project) |

### delete_track

Removes a track and its events, or (empty: true) every track that has no events.

| Argument | Type | What it is |
|---|---|---|
| `empty` | boolean | Remove every empty track instead |
| `track` | integer | Track index (see get_project) |

### envelope

Automation lines: a track's volume (0..2, 1 = 0 dB), pan (-1 left .. 1 right) or composite level (video, 0..1), in timeline seconds; or an event's velocity (speed ramp: a rate multiplier, -1 backwards .. 10, 1 = as set), in seconds from its start (the grouped sound follows). points replaces the line ([seconds, value, curve?]; curve as keyframes: linear, hold, smooth, ease_in...); active switches it on / off; clear removes it. Returns the line.

| Argument | Type | What it is |
|---|---|---|
| `active` | boolean | On or off (default: on when points are given) |
| `clear` | boolean | Remove the line |
| `event_id` | integer | velocity: the event |
| `kind` (required) | `volume` / `pan` / `composite` / `velocity` | Which |
| `points` | list of array | [[seconds, value, curve?], ...] |
| `track` | integer | volume / pan / composite: the track index |

### compound_sequence

Pack clips and their editable tracks into paired picture/audio containers. get returns an embedded timeline JSON; update replaces it as one undo step. Unpack requires an untransformed full-range container.

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `create` / `get` / `update` / `unpack` | Sequence operation |
| `event_id` | integer | Container event |
| `event_ids` | list of integer | Clips to pack; groups included |
| `name` | string | Sequence title |
| `ripple` | boolean | Move following material if full-range duration changes (default true) |
| `sequence` | object | Embedded timeline JSON from get |

### multicam

Several cameras of the same moment. sync: moves events (with their groups) so their sound lines up with the event on the topmost track (Tools > Sync Selected Events by Audio), and says which couldn't be matched. create: the video events event_ids (angle 1, 2... in that order) go under a new Multicam track on top (their tracks muted). cut: from time on, the Multicam track shows angle (what pressing its number does while playing). switch: the stretch under time shows angle. angles: the angles and the cuts. Each is one undo step.

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `sync` / `create` / `cut` / `switch` / `angles` | What |
| `angle` | integer | cut / switch: 1-9 |
| `event_ids` | list of integer | sync / create: the events |
| `time` | number | cut / switch: seconds |

### undo

*Not available to plugins (a plugin works on the open project, in one undo step).*

Undoes the last change(s), the same as Ctrl+Z.

| Argument | Type | What it is |
|---|---|---|
| `steps` | integer | How many (default 1) |

### redo

*Not available to plugins (a plugin works on the open project, in one undo step).*

Redoes undone changes.

| Argument | Type | What it is |
|---|---|---|
| `steps` | integer | How many (default 1) |

## Markers, beats and story

### add_marker

Adds a marker, or a region when it has a length.

| Argument | Type | What it is |
|---|---|---|
| `length` | number | Region length, seconds (0 = a marker) |
| `name` | string | Label |
| `time` (required) | number | Seconds |

### remove_markers

Removes the markers between two times (e.g. the beat markers once a montage is cut); regions too with regions: true.

| Argument | Type | What it is |
|---|---|---|
| `from` | number | Seconds (default 0) |
| `regions` | boolean | Also regions (markers with a length; default false) |
| `to` | number | Seconds (default: the end) |

### detect_beats

Finds the beat of the music on the timeline and puts a marker on every beat or bar. Returns the tempo.

| Argument | Type | What it is |
|---|---|---|
| `every` | integer | Marker every N beats: 1, 2, 4 (a bar), 8, 16 (default 4) |
| `from` | number | Seconds (default 0) |
| `to` | number | Seconds (default: the end; at most 15 minutes after from a call, see scanned_to) |

### beat_montage

A montage cut to the markers (Tools > Make Beat Montage; put markers on the beat with detect_beats first): a new video track on top with a clip from the video files between the cuts, each from a random part of its file.

| Argument | Type | What it is |
|---|---|---|
| `every` | integer | Cut every n markers (1, 2, 4, 8 or 16; default 4) |
| `from` | number | Seconds: only markers from here (default 0) |
| `media_ids` | list of integer | Video files to use (default: every video file) |
| `seed` | integer | For the random choices (default 1) |
| `shuffle` | boolean | Clips in a random order (default true; false: in turn) |
| `to` | number | Seconds: only markers to here (default: the end) |

### directors_dice

Seeded visual variation on selected footage/images. Keeps clip timing and sound; one undo step. Styles: measured, restless, riot.

| Argument | Type | What it is |
|---|---|---|
| `event_ids` (required) | list of integer | Selected footage/image events |
| `intensity` | number | 0 to 1 (default .6) |
| `seed` | integer | Repeatable random seed |
| `style` | `measured` / `restless` / `riot` | Visual intensity style |

### scene_changes

Finds the cuts between shots inside video events (Tools > Split Selected Events at Scene Changes) and, unless split is false, splits them (with their grouped sound) there. Returns the timeline times.

| Argument | Type | What it is |
|---|---|---|
| `event_ids` (required) | list of integer | Video events |
| `sensitivity` | number | 0..1 (default 0.5): higher finds subtler cuts |
| `split` | boolean | Split at them (default true) |

### story_chapters

Plan documentary chapters with scripts, footage/music and a narration-free listening section. Duration changes move following material together. Times are seconds.

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `list` / `add` / `adopt` / `update` / `move` / `delete` / `bind` | Chapter operation |
| `chapter_id` | integer | Existing chapter ID |
| `direction` | integer | Move -1 up or 1 down |
| `event_ids` | list of integer | Clips to assign |
| `from` | number | Adopt an existing range: start seconds |
| `listening` | number | Listening-only seconds |
| `name` | string | Chapter title |
| `narration` | number | Narration window seconds |
| `script` | string | Narration script |
| `source_in` | number | Source offset seconds |
| `source_media_id` | integer | Optional footage/music from project pool |
| `to` | number | Adopt an existing range: end seconds |

## Effects

### list_effects

The effect types: video, audio, transition or generator, with their settings (ids, ranges, choices). Search by name or category to keep it short.

| Argument | Type | What it is |
|---|---|---|
| `details` | boolean | Include every setting (default: when searching) |
| `kind` (required) | `video` / `audio` / `transition` / `generator` | Which list |
| `search` | string | Words in the name, category or description |

### add_effect

Adds an effect at the end of a chain: an event's, a track's, the video output's, the audio master's, the Live FX, a Live pad's or the Live layer's. Settings as {setting_id: value}; a value can be a number, true/false, a choice's name, "#rrggbb", text, or {"keyframes": [[seconds, value, curve?, bezier handles?], ...]} (curve: linear, hold, smooth, ease_in, ease_out, ease_in_out, bezier with [x1, y1, x2, y2], bounce, elastic; event effects: seconds from the event's start). Video effects also take mask.shape (Off/Rectangle/Ellipse/Freehand), mask.x/y/w/h (0..1), mask.feather, mask.invert; a freehand mask mask.points (3-16) and each point mask.p&lt;i>x / mask.p&lt;i>y (-0.5..0.5 across the mask's box). remove_bg removes the background with AI (model Fast or Quality); a_vst3 runs a VST3 plugin: params plugin (its file, see list_vst3_plugins) and class (for shells). Returns the effect id.

| Argument | Type | What it is |
|---|---|---|
| `effect` (required) | string | Effect type id from list_effects |
| `id` | integer | Event id (event), track index (track), pad number 1-16 (pad) or layer 1-4 (layer, default 1) |
| `params` | object | Settings to change from the defaults |
| `target` (required) | `event` / `track` / `output` / `audio_master` / `live` / `pad` / `layer` | Which chain |

### set_effect

Changes an effect's settings (same values as add_effect) or switches it on / off.

| Argument | Type | What it is |
|---|---|---|
| `effect_id` (required) | integer | From get_project / add_effect |
| `enabled` | boolean | On / off |
| `params` | object | Settings to change |

### remove_effect

Removes an effect.

| Argument | Type | What it is |
|---|---|---|
| `effect_id` (required) | integer | Effect id |

### presets

Effect presets. list: the factory and saved ones (search narrows; audio: the audio ones). apply: a preset onto a chain (target and id as add_effect), or a single-effect preset onto that effect (effect_id). save: an effect (effect_id) or a whole chain (target, id) as a preset named name. remove: a saved preset.

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `list` / `apply` / `save` / `remove` | What |
| `audio` | boolean | list: the audio presets (default: video) |
| `effect_id` | integer | apply / save: one effect |
| `id` | integer | apply / save: event id, track index, pad number or layer number |
| `name` | string | save: the preset's name |
| `preset` | string | apply / remove: its id (from list) |
| `search` | string | list: words to look for |
| `target` | `event` / `track` / `output` / `audio_master` / `live` / `pad` / `layer` | apply / save: the chain |

### node_effect

Makes video effects from a graph of nodes (Tools > Effect Builder): saved, an effect is in the User Created category, for add_effect on any event, track or adjustment layer. nodes: the node types (search narrows; with details or a search, each one's inputs and settings). preview: runs graph on the picture at time (default: the cursor) and returns the image. save: saves graph as an effect (overwrite replaces one of the same name) and returns its effect id. list: the saved effects. get: one's graph (effect: its id or name), to change and save again. A graph: {name, description, nodes: [{id, type, params: {setting: value}}], links: [[from, to, input]], exposed: [{node, param, name}]}. It needs one node of type output (and source: the picture the effect is on). Node ids are yours (whole numbers from 1). A link wires node from's output into input number `input` (from 0, in the order nodes lists them) of node to; nothing wired in reads as transparent. Setting values as add_effect's. exposed settings become the effect's own, keyframable with set_effect.

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `nodes` / `preview` / `save` / `list` / `get` | What to do |
| `details` | boolean | nodes: each one's description, inputs and settings |
| `effect` | string | get: the effect's id (user....) or name |
| `graph` | object | preview / save: the graph (see above) |
| `overwrite` | boolean | save: replace a saved effect of the same name |
| `search` | string | nodes: words to look for (e.g. blur, key, glitch, text) |
| `time` | number | preview: seconds (default: the cursor) |
| `width` | integer | preview: picture width (160..1280, default 640) |

### match_colour

Gives video events the colours of the shot at time (Tools > Match Colour to the Cursor Frame): each gets a Color Match effect (red, green and blue curves; its amount tones it down) at the end of its FX. time must be outside the events. auto: true is Auto Colour instead (Tools > Auto Colour): each event's own black and white points and white balance, from it at time (or half way through), as a White Balance and Auto Colour effect.

| Argument | Type | What it is |
|---|---|---|
| `auto` | boolean | Auto Colour each event from itself instead of matching (default false) |
| `event_ids` (required) | list of integer | Video events to change |
| `time` | number | Seconds: the frame to match (default: the cursor) |

### stabilize

Smooths camera shake out of a video event (right-click > Stabilize), analysing the file the first time. smoothing: seconds of shake smoothed (0 off, 0.5 light, 1.5 medium, 3 strong, 8 nearly a tripod).

| Argument | Type | What it is |
|---|---|---|
| `event_id` (required) | integer | A video event |
| `smoothing` | number | Seconds, 0..10 (default 1.5) |
| `zoom` | boolean | Zoom in to hide the moving edges (default true) |

### track_motion

Track Motion: an effect's mask follows what is inside it, from a time to the end of the video event, keyframing the mask's position on every frame. The effect needs a mask (add_effect params mask.shape, mask.x / y / w / h) and the event a steady forward speed. On a corner_pin effect (Track Corners): its four corners, put on the corners of a screen or sign in the shot under the event (a lower track), follow them, as keyframes on tlX/tlY, trX/trY, brX/brY, blX/blY.

| Argument | Type | What it is |
|---|---|---|
| `effect_id` (required) | integer | The masked effect on it, or its corner_pin |
| `event_id` (required) | integer | The video event (for a corner_pin, the event pinned over the shot) |
| `from` | number | Seconds on the timeline (default: the cursor, else the event's start) |

### auto_reframe

Tools > Auto Reframe for Vertical: gives video events a Fill Pan/Crop that follows the subject (a person, else the movement) for the project's frame. portrait switches a landscape project to 1080x1920 first.

| Argument | Type | What it is |
|---|---|---|
| `event_ids` (required) | list of integer | Video events |
| `portrait` | boolean | Switch a landscape project to 1080x1920 first (default false) |

## Graphics and animation

### graphics_overlay

Native scene graph and animation engine, for Graphics Overlay events (version 1 scenes: text and graphics) and Animation events (version 2 scenes; create with a motion_ preset or a v2 scene makes one; every action takes both). Actions capabilities/presets/validate/create/get/update/keyframe/bind_audio/export_template/import_template/layout/lock/brand. layout arranges selected layer_ids by operation and reference, preserving animation offsets. lock protects layer edits and descendants. brand applies a reusable font/palette kit to tagged static brand_role fills and static text fonts; locked/animated styles are preserved. v1 scenes retain their appearance; v2 adds paths (M/L/Q/C/Z commands), parented groups, symbols/instances, particles, repeaters, charts, camera, masks, gradients, shadows/glow, trails/motion_blur and character/word/line text_animation. Layer shading (0-1, light from shading_angle, default 225 = top left), contact_shadow (0-1), blur (fraction of height) and depth (0-1, blurred by camera.depth_of_field as the camera zooms in); fixed: true keeps a layer (and a group's children) still on the picture when the camera moves (lyrics, captions, logos); scene motion_blur {samples, shutter seconds}. A track binding's visemes give mouth shapes: {audio:binding, viseme:A\|E\|O\|L\|F\|M\|rest} is 1 while speaking that shape. Characters (action characters / add_character) come with such mouths; source_track bindings follow later edits of the track. Their expression (action expression: neutral, angry, sad, surprised, smug) is the scene variable &lt;id>_mood. Sets (action sets / add_set) are ready backgrounds, replaced by the next add_set. Layer type video {path, source_in, speed, loop, fit: cover\|contain} plays a file inside its box; a layer's effects [{type: video effect id, params}] apply to its own picture only. camera.speaker {zoom (1.7), window (0.5 s), quiet (0.06)} frames whichever character (a layer whose id matches its binding) is loudest. patch sets or removes properties by dotted path (operations [{layer_id, property, value \| remove}]) without sending the scene; batch runs many move/expression/keyframe/patch steps as one undo step (both answer briefly). props / add_prop (layer_id: a character, prop, hand R\|L) put a prop in a hand; it moves with the arm. dance keys moves on the beats of the music under the scene (dancers, moves, every). Animate properties with {keyframes:[[event-local seconds,value,easing?],...],easing,loop,ping_pong}; numeric, colour, discrete and compatible path morph values work. follow:{path:layer_id,progress,orient,loop} follows curved paths. Scene variables substitute {{name}} in text; datasets drive charts. Colours are #RRGGBB, or #AARRGGBB with the alpha FIRST (#00000088 is clear: use opacity for see-through). bind_audio embeds real source envelopes and native detected beats (source_track: every audio event on a track during the scene, e.g. one binding per character's voice track for lip sync); property {audio:binding,base,amount,channel:peak\|left\|right\|beat} reacts without adding sound. Read capabilities and user_guide page animation-scenes.md for limits and examples. All edits are saved and undoable.

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | string | capabilities, presets, validate, create, get, update, keyframe, bind_audio, export_template, import_template, layout, lock, brand, move, characters, add_character, save_character, expression, sets, add_set, props, add_prop, patch, batch, dance |
| `amount` | number | move: how big (default 1) |
| `at` | number | Timeline start, seconds |
| `binding` | string | Audio binding name (default source) |
| `bins` | integer | Envelope bins: 64â€“8192 |
| `brand` | object | version:1, name, font and primary/secondary/accent/background/foreground colours |
| `character` | string | add_character: a name or file from action characters |
| `colours` | object | add_character: their colour settings, e.g. {jacket: '#d7263d'} |
| `dancers` | list of string | dance: the characters' layer IDs; on each beat of the music under the scene each does the next move (from, to: event-local seconds; every: beats, default 1; moves: names, default hop, spin, jump, squash, arms up, shake) |
| `duration` | number | Audio source span, seconds |
| `easing` | any | Curve name or [x1,y1,x2,y2] |
| `event_id` | integer | Existing graphics event |
| `every` | integer | dance: a move every this many beats (default 1) |
| `from` | number | dance: event-local seconds to start from (default 0) |
| `ground` | number | add_character: where their feet are, 0 to 1 (default 0.9) |
| `hand` | string | add_prop: R (default) or L |
| `layer_id` | string | Layer ID for keyframe, or $camera/$variables |
| `layer_ids` | list of string | Layer IDs for layout or lock; get returns addressable IDs, including legacy scenes |
| `length` | number | Event duration, seconds |
| `locked` | boolean | Protect selected layers (true), or unlock them (false) |
| `media_id` | integer | Imported source media |
| `mood` | string | expression: neutral, angry, sad, surprised or smug, for character layer_id from time on |
| `move` | string | move: hop, jump, shake, nod, wave, spin, walk, pop, squash, blink, point, arms up or shrug, on layer_id at time |
| `moves` | list of string | dance: the moves, in turn |
| `name` | string | add_character: who they are (their binding is the name in lower case); save_character: its library name |
| `operation` | string | left, hcenter, right, top, vcenter, bottom, distribute_h, distribute_v |
| `operations` | list of object | patch: [{layer_id ($camera, $variables, $scene too), property (dotted path), value \| remove: true}]; batch: [{action: move\|expression\|keyframe\|patch, its other arguments}], in order, as one undo step |
| `overwrite` | boolean | Allow overwriting an exported template |
| `params` | object | Generator opacity |
| `path` | string | Native .json template path |
| `preset` | string | Existing broadcast layouts or motion_race/titles/chart/route/particles/morph/audio/reusable (motion_ prefix) |
| `prop` | string | add_prop: a name or file from action props, put in character layer_id's hand |
| `property` | string | Property path, e.g. x, fill, follow.progress |
| `reference` | string | canvas (default), safe or selection |
| `scene` | object | Complete editable scene; update replaces it |
| `set` | string | add_set: a name or file from action sets |
| `source_event_id` | integer | Straight audio/video source event |
| `source_in` | number | Audio source offset, seconds |
| `source_track` | integer | Track index: every audio event on it during this scene (no beats) |
| `time` | number | Keyframe event-local seconds |
| `to` | number | move walk: the x to walk to |
| `track` | integer | Preferred video track index |
| `value` | any | Keyframe value |
| `waveform_event_id` | integer | Reuse an existing waveform envelope |
| `x` | number | add_character: left edge, 0 to 1 |

### waveform_graphic

Draws real source audio as a native waveform. Actions create/get/update/refresh. Supply source_event_id or media_id. Audio peaks are decoded once and embedded in the project. source_in and duration are source seconds; length is timeline duration. Source-event speed is reflected in progress; reverse, loops and speed ramps are rejected. Shows source audio before effects, not a master mix. It does not add audio. params control x,y,width,height,color,played_color,style (Bars/Filled envelope/Line),stereo,normalize,progress,scroll,window,amplitude,rate,offset.

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | string | create, get, update, refresh |
| `at` | number | Timeline start, defaults to source event start or cursor |
| `bins` | integer | Peak envelope resolution: 64â€“8192 (default 2048) |
| `duration` | number | Source span up to 3600 seconds |
| `event_id` | integer | Waveform event for get/update/refresh |
| `length` | number | Timeline duration, defaults to source event length or source span |
| `media_id` | integer | Imported media with audio |
| `params` | object | Waveform style settings; numeric settings support keyframes |
| `source_event_id` | integer | Existing audio or video event with audio |
| `source_in` | number | Source offset, seconds |
| `track` | integer | Preferred video track index |

## Sound, voices and captions

### list_sound_effects

The built-in sound effects library (whooshes, booms, dings, comedy stings...), all CC0: category, name and file path for import_media or add_media_to_timeline.

| Argument | Type | What it is |
|---|---|---|
| `search` | string | Only names containing this |

### normalize_audio

Edit > Normalize Audio Events: each audio event's loudest peak to -0.1 dBFS (a Gain effect at the front of its chain).

| Argument | Type | What it is |
|---|---|---|
| `event_ids` (required) | list of integer | Audio events |

### audio_finishing

Measure the entire final stereo mix after all audio effects using EBU R128 and oversampled true peaks. analyze returns LUFS, LRA, dBTP, sample peaks, stereo checks, silence and warnings. apply_gain reanalyzes and adds an undoable master gain (in front of a Master FX Limiter if there is one, up to the loudness target; else at the end,) limited by true-peak headroom; reanalyze afterward. A cancellable progress window appears. Targets are editable, not platform guarantees.

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | string | analyze or apply_gain |
| `lufs` | number | Target -36 to -5 LUFS (default -16) |
| `true_peak` | number | Ceiling -12 to 0 dBTP (default -1.5) |

### separate_stems

Right-click > Separate Stems (the Stem Separation engine: Tools > AI Components): start splits audio event event_id into voice and background (stems 2) or vocals, drums, bass and other (stems 4), in the background; status says how far it is, then the new events (each stem on a track under the event, which is muted). best: the fine-tuned model, about 4x slower. cancel stops it.

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `start` / `status` / `cancel` | What |
| `best` | boolean | start: the slower, better model (default false) |
| `event_id` | integer | start: an audio event from a file |
| `stems` | integer | start: 2 or 4 (default 2) |

### captions

Captions on a new Captions track: auto (speech to text with Whisper over the time range), import (an SRT / WebVTT file) or export (the captions track to SRT / WebVTT). auto needs the model on this computer (Tools > Captions > Auto Captions downloads them; the offline release has them all).

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `auto` / `import` / `export` | What |
| `from` | number | auto: seconds (default 0) |
| `language` | string | auto: "auto" (default), "en", "de"... |
| `max_chars` | integer | auto: longest caption (default 42) |
| `model` | `tiny` / `base` / `small` / `speakers` | auto: the speech model (default base; speakers = English, marks who is speaking) |
| `path` | string | import / export: the .srt or .vtt file |
| `style` | `subtitles` / `kinetic` | auto / import: how they look (default subtitles) |
| `to` | number | auto: seconds (default: the end) |
| `translate` | boolean | auto: into English |

### list_voices

Lists the local Kokoro voice catalog, licenses and installed/download status. No internet access.

No arguments.

### generate_voice

Starts local AI speech from a captions track. Adds a new audio track as one undo step when finished. Download voices first in Tools > Captions > Generate Voice. Poll voice_status. Do not edit or switch projects until it finishes.

| Argument | Type | What it is |
|---|---|---|
| `align_captions` | boolean | Generate captions with actual local Whisper word timestamps |
| `caption_event_id` | integer | A text event on the captions track (default topmost text track) |
| `chapter_id` | integer | Chapter to own generated voice and captions |
| `device` | `auto` / `cpu` / `gpu` | Speech inference device (default auto, GPU preferred with CPU fallback); RVC supports its own device field |
| `from` | number | Only captions starting at or after this second |
| `group` | boolean | Join short cues into sentences (default true) |
| `leading_pause` | number | Real silence in seconds, 0 to 10; inline [pause=0.5] also supported |
| `pronunciations` | object | Word to spoken pronunciation dictionary |
| `rate` | number | Speech rate 0.5 to 2 (default 1) |
| `replace_event` | integer | Regenerate one existing audio narration event; retain earlier takes |
| `ripple` | boolean | Ripple following events when replacement length changes |
| `rvc` | object | Optional bundled RVC conversion: model (exported .pth), index, pitch, index_rate, protect, speaker_id, f0_method (rmvpe or pm). No Python or RVC installation needed; shared support models must be downloaded in Generate Voice first. |
| `script` | string | Speak this script instead of a captions track |
| `sentence_pause` | number | Real silence in seconds, 0 to 10; inline [pause=0.5] also supported |
| `start` | number | Script start seconds |
| `timing` | `fit` / `natural` | Fit up to 1.35x faster, or natural duration |
| `to` | number | Only captions starting before this second |
| `trailing_pause` | number | Real silence in seconds, 0 to 10; inline [pause=0.5] also supported |
| `voice` | string | Kokoro voice ID from list_voices (default af_heart) |
| `window` | number | Script narration window seconds (default 15) |

### generate_dialogue

Speaks a script of "Name: line" lines with a voice per character, each on their own new audio track named after them (for lip sync: graphics_overlay bind_audio with source_track), lines in script order, as one undo step. Poll voice_status (its dialogue field). Do not edit or switch projects until it finishes.

| Argument | Type | What it is |
|---|---|---|
| `align` | boolean | Time each word with the Auto Captions model for exact mouth shapes (default true when a model is downloaded) |
| `cast` | object | Per character name: {voice (list_voices id, default af_heart), rate 0.5 to 2, pitch semitones -24 to 24} |
| `gap` | number | Seconds between lines, 0 to 10 (default 0.3) |
| `script` (required) | string | One line per speech: Name: what they say. A line (pause 2) adds two seconds of silence before the next; [radio], [phone], [megaphone], [echo], [hall], [dream] or [backwards] at the start of a line gives it that sound (not spoken) |
| `start` | number | Seconds (default the cursor) |

### voice_status

Progress and result of generate_voice, including timing warnings and output folder. cancel stops the worker.

| Argument | Type | What it is |
|---|---|---|
| `cancel` | boolean | Cancel generation |

### list_vst3_plugins

The VST3 audio plugins installed on this computer, with the effects in each (for a_vst3's plugin / class).

No arguments.

### ai_components

Lists optional Windows AI engines. action install downloads hash-verified dependencies directly from Astral, PyPI and PyTorch into private app environments. Visible setup progress supports cancellation; failed repairs preserve existing engines.

| Argument | Type | What it is |
|---|---|---|
| `action` | string | list (default) or install |
| `component` | string | speech or rvc |
| `device` | string | cpu (default) or cuda; CUDA speech uses libraries from CUDA RVC |

## Looking and playing

### get_frame

The picture at a time (default: the cursor), as an image, with the effects.

| Argument | Type | What it is |
|---|---|---|
| `save_to` | string | Also save it to this file (.png or .jpg; File > Export Frame), at the project's size unless width is given |
| `time` | number | Seconds |
| `width` | integer | Image width, 160-1920 (default 640) |

### transport

Moves the cursor, plays or stops the preview, or sets the time selection (select: from-to; to at or before from clears it). loop (with any action) switches Loop Playback: Play goes round the time selection until stopped.

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `seek` / `play` / `stop` / `select` | What |
| `from` | number | select: seconds |
| `loop` | boolean | Loop Playback on or off |
| `time` | number | seek: seconds |
| `to` | number | select: seconds |

## Rendering and delivery

### render

Renders the project (or a part) to a video file in the background (or in place onto a Rendered track, or as stems); render_status tells how far it is. Won't overwrite a file unless overwrite is true.

| Argument | Type | What it is |
|---|---|---|
| `from` | number | Seconds |
| `hardware_encoding` | boolean | Prefer an available GPU encoder (default true); false preserves the selected template encoder |
| `in_place` | boolean | Render from-to (the picture as it is, without Output FX) to a ProRes file that goes onto a Rendered track on top when done, so it plays smoothly (Tools > Render Selection to New Track); no path or preset |
| `overwrite` | boolean | Replace an existing file |
| `path` | string | Output file, e.g. C:/Users/me/Videos/out.mp4 (stems: the folder) |
| `preset` | string | Words from a Render As template name (default: MP4 H.264) |
| `smart_render` | boolean | Copy the video where it is plain cuts of files already in the template's codec, size and frame rate (MP4/MOV); render_status says whether it did |
| `stems` | boolean | Each audio track with sound to its own 24-bit WAV in the folder path, after its effects, level, pan and ducking, before Master FX (File > Export Stems) |
| `to` | number | Seconds |

### render_status

How the render started with render is going (progress 0..1, done, error); cancel stops it.

| Argument | Type | What it is |
|---|---|---|
| `cancel` | boolean | Stop it |

### render_queue

File > Render Queue: renders one after another in the background. add queues the open project as it is now (or project_file, without opening it); start, stop (cancels the running one); status lists the jobs.

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `add` / `start` / `stop` / `status` | What |
| `from` | number | add: seconds |
| `hardware_encoding` | boolean | add: prefer an available GPU encoder (default true) |
| `path` | string | add: output file |
| `preset` | string | add: words from a Render As template name (default: MP4 H.264) |
| `project_file` | string | add: another .mvp to render instead of the open project |
| `to` | number | add: seconds |

### deliver

The Deliver tab: getting a finished edit out. check (readiness, the checks with what is worth a look, the outputs with files and size estimates, the publish pack; measure_loudness scans the mix first), outputs (change the outputs: add / remove templates or a pack, tick, trim to a platform limit, folder; files are named after the project and template, with the right extension), render (every ticked output through the render queue; render_queue status follows it), pack (set the title, description with {chapters} {tracklist} {title} {length} {date} placeholders, tags, the thumbnail frame; save writes description, thumbnail and captions to the folder) or history (past renders of the project, with their lengths checked).

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `check` / `outputs` / `render` / `pack` / `history` | What |
| `add` | list of string | outputs: Render As templates to add (words from their names) |
| `description` | string | pack: the description; placeholders are filled in when saved |
| `folder` | string | outputs: the folder the files go in |
| `loudness_target` | integer | check: 0 online video / speech -16, 1 louder online -14, 2 EBU -23, 3 US -24 |
| `measure_loudness` | boolean | check: scan the final mix's loudness first (takes a while) |
| `pack` | `youtube` / `social` / `audio` / `masters` / `share` | outputs: add a pack of templates |
| `remove` | list of string | outputs: templates to take out (words from their names) |
| `save` | boolean | pack: write the description, thumbnail and captions into the folder |
| `tags` | string | pack: comma-separated tags (YouTube takes 500 characters) |
| `thumbnail_time` | number | pack: seconds; that frame becomes the thumbnail |
| `ticked` | list of string | outputs: tick only these (words from their names); the rest are unticked |
| `title` | string | pack: the video title (drawn on the thumbnail) |
| `trim` | object | outputs: {template words: seconds} renders only the start (0: the whole edit) |

### youtube_publish

For the upload: chapters ("00:00 Name" lines from story chapters or markers, with warnings when YouTube wouldn't show them), track_listing (an audio track's events as "01. Name - 04:19") or thumbnail (a 1280x720 JPEG of the frame at a time with an optional title, written to path).

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `chapters` / `track_listing` / `thumbnail` | What |
| `path` | string | thumbnail: the .jpg or .png to write |
| `time` | number | thumbnail: seconds (default: the cursor) |
| `title` | string | thumbnail: words on the picture |
| `track` | integer | track_listing: audio track index |

## Live

### live_status

The Live tab: capturing or not, the sources to choose from, the pads (names, effects), which is on, scenes, the setlist, the tempo, recording (last_recording: the latest clip's file) / streaming / Spout / virtual camera, the cue.

No arguments.

### live_action

Plays the Live tab: start / stop capturing, switch a pad on or off, recall a scene, a setlist song (or the next / previous), tap the tempo, Set 1, find the tempo again, lock / unlock it, start / stop recording, save the replay, cue a pad or scene and take it. (Going live on a streaming site is left to the person.)

| Argument | Type | What it is |
|---|---|---|
| `action` (required) | `start` / `stop` / `pad` / `scene` / `song` / `next_song` / `previous_song` / `tap` / `set_one` / `find_tempo` / `lock_tempo` / `unlock_tempo` / `record` / `stop_recording` / `save_replay` / `cue_pad` / `cue_scene` / `take` / `close_cue` | What |
| `number` | integer | pad 1-16, scene 1-8, song 1-N |

### set_live_source

Chooses the Live tab's picture source and / or audio input by (part of) its name (see live_status).

| Argument | Type | What it is |
|---|---|---|
| `audio` | string | Audio input name, or "none" |
| `video` | string | Camera, screen, window or video name |

### set_tempo

Sets the Live tempo (BPM), locked or as a starting point to listen from.

| Argument | Type | What it is |
|---|---|---|
| `bpm` (required) | number | Beats per minute |
| `lock` | boolean | Lock it (default true) |

### set_pad

Fills a Live pad: a name and an effect chain (each {"type": id, "params": {...}}), replacing what was there; or just renames it / makes it momentary. Locked pads are left alone.

| Argument | Type | What it is |
|---|---|---|
| `effects` | list of object | The chain, first to last |
| `momentary` | boolean | On only while held |
| `name` | string | Label |
| `pad` (required) | integer | 1-16 |

### save_scene

Saves the current Live look (Live FX, pad, layer, overlays) as a scene.

| Argument | Type | What it is |
|---|---|---|
| `name` | string | Label |
| `scene` (required) | integer | 1-8 |

### live_setlist

The Live tab's setlist (Page Down / Page Up play its songs): without songs, lists it; songs replaces it, each {name, bpm (0: listen for it), lock_bpm (default true), scene (1-8, 0: none), pad_set (a saved pad set's name, "": keep the pads), notes}. One undo step.

| Argument | Type | What it is |
|---|---|---|
| `songs` | list of object | The new setlist, in order (empty: none) |

### live_switcher

The Live switcher: the inputs kept open beside the Source (input 1), and which one is on air. inputs sets inputs 2-5 by (part of) their names (see live_status; "" = off); cut puts an input on air at once, auto with the pads' transition, preview picks the next one.

| Argument | Type | What it is |
|---|---|---|
| `action` | `cut` / `auto` / `preview` | What to do with input |
| `input` | integer | 1-5 (1 = the Source) |
| `inputs` | list of string | Inputs 2-5, up to four names |

### get_live_frame

The Live output as it is now (with effects and overlays), as an image.

| Argument | Type | What it is |
|---|---|---|
| `width` | integer | 160-1920 (default 640) |
