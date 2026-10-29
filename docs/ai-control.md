# AI Control (MCP)

**Options > Allow AI Control (MCP)** lets an AI assistant on this computer work in Mutant Video Producer through the
[Model Context Protocol](https://modelcontextprotocol.io): Claude Code, Claude Desktop, or any MCP
client. The assistant sees the project and edits it with you, step by step, while you watch.

## Setting it up

1. Tick **Options > Allow AI Control (MCP)**.
2. Open **Options > AI Control Setup...**. It shows the lines to paste, with the right paths for
   your computer:
   - **Claude Code**:

     ```
     claude mcp add --transport http mvp http://127.0.0.1:47811/mcp
     ```

   - **Claude Desktop**, and other clients that start a program: `MutantVideoProducer.exe --mcp-stdio` as the
     command in `claude_desktop_config.json`. It passes messages on to the Mutant Video Producer that is open.
3. Ask the assistant to do something, for example *"put a crossfade between every clip on the
   first track"* or *"add captions and make them yellow"*.

Every edit the assistant makes is one *AI: ...* step on the undo stack, so **Ctrl+Z** takes it back.
The status bar shows each tool it uses.

`collect_project` takes `destination` (a new folder), `include_unused_media` and `include_voice_models` (both default true). It includes unsaved edits without changing the open project and returns `project`, `report`, `copied_files`, `copied_bytes`, `missing` and `warnings`. Its copying progress window appears even for MCP calls and can cancel the operation.

## Making a whole video

The assistant is told to read [Making a whole video through AI control](ai-workflow.md) (through `user_guide`) before
it makes a whole video: the order of the work (sound first, a scene per shot, titles, checks, finishing), the tools
for each step and the checks that catch mistakes. It is how the *Snowbank* episodes were made.

## What it can do

**The Editor**

| Tool | What it does |
|---|---|
| get_project | Reads the media, tracks, events, effects and their settings, markers. |
| user_guide | Reads this User Guide and the tutorials: the list of pages, or one page. |
| multicam | Lines events up by their sound (sync), makes a multicam of video events (create), cuts or switches between angles, and lists the angles and cuts. |
| graphics_overlay | Compose native layered text, shapes and images; create/read/update broadcast presets or custom scenes. `bind_audio` with `source_track` (a track index) and `binding` (a name) binds every sound on that track during the scene, for lip sync from each character's voice track (it follows later edits of the track, and its `visemes` drive mouth shapes). `characters` lists the character library, `add_character` adds one (`character`, `name`, `x`, `colours`), `save_character` saves a group; `expression` sets a character's face (`layer_id`, `mood`, `time`); `move` adds a ready-made move or gesture (`move`, `layer_id`, `time`, `duration`, `amount`, `to`); `sets` lists the set library and `add_set` (`set`) puts one behind the scene; `patch` changes properties by path (`operations`), `batch` runs many move/expression/keyframe/patch steps as one undo step, `dance` keys moves on the music's beats (`dancers`, `moves`, `every`), and `props` / `add_prop` put a prop in a character's hand. `freeze_frame` `in_place` holds a frame over the following time without moving anything later. Layers can be `video` (`path`, `source_in`, `speed`, `loop`, `fit`) and carry their own `effects`; the camera's `speaker` frames whoever is talking. Colours are `#RRGGBB`, or `#AARRGGBB` with the alpha first. |
| waveform_graphic | Build and style native graphics from real source audio, with embedded peaks, stereo and playback progress. |
| import_media, add_media_to_timeline | Imports files and places them. |
| add_text | Adds a title, text or any generator preset; `params` sets the generator's own settings (a title's font, size and colour, a starfield's speed). |
| edit_event, split, delete_events, add_track | Moves (also to another track), trims, splits and deletes events, with their grouped sound; `edit_event` `params` changes a generator's settings. `add_track` with no index puts a video or image track on top and an audio track at the bottom. Edits that would change a locked track are refused. |
| list_effects | Lists every effect with its settings. |
| add_effect, set_effect, remove_effect | Effects (with keyframes and masks) on events, tracks, the output, the audio master, the Live FX, pads and the layer. Remove Background, freehand masks and VST3 plugins work through these. |
| list_vst3_plugins | The VST3 plugins installed. |
| add_marker, detect_beats | Markers, and markers on the beat. |
| captions | Auto Captions (any model on this computer, translation, speakers), and importing or exporting SRT / WebVTT. |
| list_voices | Curated Kokoro voices, metadata, installed status, and local worker/model availability. |
| list_sound_effects | The built-in sound effects library (CC0): category, name and file path of each sound, optionally filtered by `search`; add one with `import_media`. |
| generate_dialogue | Speaks a `Name: line` script with a voice, rate and pitch per character (`cast`), one new audio track per character named after them, lines in script order from `start` with `gap` between them, as one undo step; `align` (default on when a Whisper model is installed) times each word for exact mouth shapes. Poll `voice_status` (its `dialogue` field). |
| generate_voice, voice_status | Starts local caption-to-speech generation and polls or cancels it; optional bundled RVC conversion with an imported `.pth` model. A completed job adds one undoable Voice track. |
| ai_components | Lists optional Windows speech/RVC engines and upstream sources; installs CPU or CUDA environments from Astral/PyPI/PyTorch. Installation shows progress, verifies locked wheels and needs no separate Python install. |
| get_frame | Looks at any frame as an image. |
| transport | Plays, stops and moves the cursor. |
| render, render_status | Renders in the background with any Render As template; opens the detailed Render Status window and exposes statistics in `render_status.details`. `in_place` renders from-to onto a Rendered track on top (Tools > Render Selection to New Track); `stems` writes each audio track to a WAV in the folder `path` (File > Export Stems). |
| undo, redo, save_project, set_project_settings | Undo, save, frame size and rate. |
| collect_project | Creates a portable project/media folder, opens copying progress, and returns the project, report, missing files and dependency warnings. |

**The Deliver tab**

| Tool | What it does |
|---|---|
| deliver | `check`: whether the edit is ready, the checks worth a look, the outputs with their files and size estimates, and the publish pack (`measure_loudness` scans the mix first). `outputs`: add or remove templates or a pack (youtube, social, audio, masters, share), tick, trim to a platform limit, set the folder. `render`: every ticked output through the render queue. `pack`: title, description (with `{chapters}`, `{tracklist}`, `{title}`, `{length}`, `{date}`), tags and the thumbnail frame; `save` writes them with the captions. `history`: past renders, lengths checked. |

**The Live tab**

| Tool | What it does |
|---|---|
| live_status | Reads everything on the Live tab. |
| set_live_source | Chooses the sources. |
| live_action | Start / stop, pads, scenes and setlist songs, filling pads, recording, cue and take. |
| set_tempo, set_pad, save_scene | Tempo, pad chains, scenes. |
| live_switcher | The switcher's inputs, preview, Cut and Auto. |
| get_live_frame | Looks at the output. |

## Safety

- Only programs on this computer can connect (127.0.0.1). Requests from web pages are refused.
- **Going live on a streaming site is left to you**: there is no tool for it.
- The stream key is never shown to the assistant.
- Turn it off again with **Options > Allow AI Control (MCP)**.

## Try it

The [Letting an AI Assistant Edit](tutorials/16-ai-assistant.md) tutorial walks through a session.

## Documentary tools

`audio_finishing` measures final-mix LUFS, loudness range and true peaks, reports mix warnings and can apply safe master gain (in front of a Master FX Limiter when there is one). See [Audio finishing](audio-finishing.md).
`graphics_overlay` also exposes alignment, distribution, saved locks and reusable brand kits. See [Animation scenes](animation-scenes.md#layout-and-brand-workflow).

`story_chapters` lists, adds, adopts, updates, reorders, assigns or deletes chapters. Durations are seconds; `get_project.chapters` contains saved microsecond strings. A chapter has a narration window followed by listening-only time. `adopt` turns an existing `from`/`to` range into a chapter, splitting boundary clips and retaining their sync.

`compound_sequence` packs `event_ids`, returns its embedded timeline with `get`, replaces that content with `update`, and restores editable tracks with `unpack`. The JSON returned by `get` uses the same microsecond string fields as project files. Import media into the parent pool first. Paired picture/audio containers share the same immutable child content; updating either updates its linked partner. `ripple` defaults to true.

`generate_voice` also accepts a standalone `script`, `start` and `window`, or `replace_event` to regenerate one narration event. Earlier takes remain in the event's `narration.takes`. `pronunciations` maps spellings to spoken forms; `leading_pause`, `trailing_pause`, `sentence_pause` and inline `[pause=0.5]` create real silence. `align_captions: true` uses an installed local Whisper model to create word captions and retain their timing with each take. `chapter_id` reserves the listening section: an overlong take is retained on disk but not inserted. RVC conversion remains available through `rvc`.

GPU acceleration is automatic where available. `generate_voice.device` and
`rvc.device` accept `auto`, `gpu`, or `cpu`; `voice_status.backends` reports the
actual engines. `render.hardware_encoding` defaults to true and prefers a usable
hardware encoder; false preserves the chosen template's encoder. Render status
includes `color_conversion`. See [GPU acceleration](gpu-acceleration.md) for
supported stages and fallbacks.

`directors_dice` accepts `event_ids`, `seed`, `style` (`measured`, `restless`, `riot`) and `intensity` (0–1). It changes selected footage/image motion and visual accents without changing sound or clip timing. Each call is one undo step.

While a sequence or Dice preview window owns the timeline, MCP mutation calls return an explanation; finish or cancel that window before editing remotely.
