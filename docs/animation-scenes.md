# Animation scenes

Mutant Video Producer's graphics composer now has a general 2D animation engine. Existing version-1 graphics retain their original renderer and appearance. New animation presets use version 2. Animation, audio analysis and reusable definitions are embedded in the project and use the same renderer in preview, CPU/GPU composition and export.

## Using the composer

Drag an **Animation** preset, or **Blank Graphics Canvas**, from the Generators / Graphics category onto the timeline. Select it and choose **Compose Graphics…** in Properties. The [animated short tutorial](tutorials/17-animated-short.md) builds a talking character from scratch.

- **Layout** edits text, colour and geometry. Select several layers to group them. Parent groups move, rotate and scale their children; their anchors can serve as pivots for articulated objects. **Make reusable object** turns a layer/group into a definition with editable instances.
- **Motion** edits parent groups, path following, scale/anchors, path reveal, typography, masks, gradients, shadows, glow, trails, motion blur, particles/repeaters, charts and the scene camera. Add a Path, enable **Edit curve points on canvas**, then drag endpoints/control handles. The path buttons append, curve and close segments.
- **Keys** selects a layer property, camera property or scene variable. Scrub to a time, enter a value and choose **Set key at playhead**. Drag diamonds on the timeline to retime keys. Keys can hold numbers, colours, text or compatible path geometry. Easing belongs to the outgoing segment. **Record keyframes** records changes made in the other controls, while editing an already animated property preserves its animation.
- **Scene** imports chart JSON/CSV and binds real audio envelopes and beats. Advanced scene/layer data exposes the complete schema, including reusable definitions and audio mappings. Apply data to preview before continuing layout work; Apply also validates pending data drafts.
- **Play preview** and the scrubber show event-local animation. The composer previews a 16:9 canvas; export uses the project's actual canvas.
- **Open/Save template** exchanges native JSON compositions. The default template folder is your profile's `Media/Graphics Templates`. Image references are retained, not copied; embedded shapes, definitions and audio envelopes need no external artwork.

Presets: curved route with reusable scene objects, animated typography, data chart, revealed route, deterministic confetti, morphing shapes, audio-reactive shapes/bars, reusable information cards, and two cut-out **characters** whose mouths, arms and bodies follow the audio bindings `voice_a` and `voice_b` (bind each character's voice track under those names; their hat, band, jacket, mitten and skin colours are the scene variables `a_hat`, `b_jacket` and so on). These are editable starting points.

## Characters

**Add character...** (under the layer list) puts a ready-made cut-out character into the scene: kids (with a pompom
hat, long hair, ear flaps or a hood), a man with a cap and beard, a teacher, a woman and a bear. Give them a name, such
as `Gus`:

- Their layers' ids start with their name (`gus`, `gus_head`, `gus_mouth_O`...), so several can share a scene; a
  second Gus becomes `gus_2`.
- Their colours are scene variables (`gus_jacket`, `gus_hat`, `gus_skin`...): change them in **Keys** with **Scene
  variables**, or in Scene data.
- Their voice binding is their name in lower case. Bind their voice track under it (**Scene > Bind audio for
  animation...**, the track, binding name `gus`) and they talk: the mouth opens with the voice, the arms gesture and the
  body bobs.
- **Mouth shapes**: the mouth takes the shape of the words (open A, wide E, round O, small L, teeth-on-lip F, closed M
  for m/b/p, and a resting mouth when quiet). The shapes come from each line's words: a generated voice's text (Generate
  Voice, Generate Dialogue), spread over the sound of the line, or its word times when the take was aligned.

**Save as character...** saves the selected group (a character you made or changed) to your library, in your profile's
`Media/Graphics Templates/Characters`; it then appears in Add character in every project. Its fill colours become its
settings and its most used audio binding becomes its voice.

Make your own talking mouth with layers whose opacity is `{"audio": "gus", "viseme": "O"}`: it is 1 while Gus is
saying an O, and 0 otherwise. The shapes are `A`, `E`, `O`, `L`, `F`, `M` and `rest`.

**Expressions.** Library characters have five faces: **neutral**, **angry**, **sad**, **surprised** and **smug**
(brows and eyelids). On the Keys tab, select the character's own group, choose one in the **Expression** box and press
**Set expression**: the face changes at the playhead and stays until the next one. The expression is the scene variable
`gus_mood`, so it can also be keyed with **Scene variables**.

**Subtitles.** A text layer whose `text` is `{"subtitle": "gus"}` shows the words of whichever line is playing on Gus's
bound voice track, and nothing between lines (`"upper": true` for capitals, `"hold"` for how long the words stay after
the line, 0.3 s by default). It suits backwards speech, a foreign line, or a karaoke card.

**Lip sync follows edits.** A binding made from a track remembers the track. Move, trim, replace or delete a line on it
(or move the scene) and the mouth's analysis is redone as part of the same edit, so it never drifts from the voice; one
undo puts both back.

## Moves

The **Moves** box on the Keys tab adds a ready-made move to the selected layer at the playhead, landing back on its own
animation: **hop**, **jump**, **shake**, **nod**, **wave** (on an arm's group, which turns at its anchor), **spin**,
**walk** (to the x you give, with a bob in each step), **pop** (scales in), **squash** and **blink** (on an eyes group).
**Length** 0 uses the move's natural length; **Amount** makes it bigger or smaller (negative: the other way).

Gestures go on a library character's own group and turn its arms, then bring them back: **point** (the right arm),
**arms up** (both, for a cheer or a panic) and **shrug**.

## Sets

**Add set...** (under the layer list) puts a ready-made background behind everything in the scene: a classroom, a
bedroom, a cottage kitchen, a snowy street, woods with a cave, a western main street, mission control, the inside of
a rocket, outer space and more. Some sets have a front part (a
desk, a counter, a window frame) drawn in front of the characters. A scene has one set: adding another replaces it.
The set's layers are grouped as `set` (and `set_front`), with a depth of 1, so **Depth of field** softens them in a
close-up. A set's signs (the town hall banner, the classroom board, the cafeteria menu, the shop fronts) are scene
variables named `set_banner`, `set_chalk`, `set_menu` and so on: reword them in the Keys tab's Scene variables (or with a
`patch` of `$variables`). Your own sets go in your profile's `Media/Graphics Templates/Sets`, as JSON files with `name`, `layers` and an
optional `front` list of layers.

## Video in a scene

Choose **video** in the layer picker and press **Add layer**, then **Choose file...** on the Layout tab: the video plays
inside the layer's box, so it can be a TV, a phone, a window or a porthole, moved, scaled, turned and animated like any
layer. In Layer data, `fit` is `cover` (fills the box, cropped; the default) or `contain` (all of it, letterboxed),
`source_in` is where in the file it starts (seconds), `speed` its playback speed and `loop` whether it starts again at
its end. The picture only: put its sound on an audio track.

## Effects on one layer

**Effect on this layer...** (under the layer list) applies a video effect to the selected layer (or group) only: a glow
on a sign, a blur or tint on the background, an old-film look on a TV screen. The layer's `effects` list in Layer data
holds them, each `{"type": effect id, "params": {setting: value}}` with the settings of the effect of that id (see
`list_effects`); choose **(remove this layer's effects)** to clear them.

## The look

On the Layout tab, every shape has:

- **shading**, 0 to 1: soft light from the top left (`shading_angle` in degrees changes where it comes from: 225 is the
  top left, 270 above), a highlight fading into a shadow across the shape.
- **contact shadow**, 0 to 1: a soft shadow on the ground under the layer's box.
- **blur**: softens the layer (a fraction of the picture's height).
- **depth**, 0 to 1: how far back the layer is. With **Depth of field** on the Scene tab, far layers blur as the camera
  zooms in (a close-up softens the background); 0 is the subject's plane.

The Scene tab's **Look** box also turns on **Motion blur**: the whole picture, camera included, is averaged over the
shutter time, so crash zooms, whip pans and fast hits smear. It draws each frame five times; the preview while playing draws fewer (draft quality: motion
blur, layer motion blur and layer effects are lighter), and renders always draw them in full.

**Camera frames whoever is talking** (Look box) cuts the camera to the character who is talking: the one whose voice
binding has been loudest over the last half second, framed head and shoulders at the **Close-up zoom**. A character is a
layer (their group) whose id is their binding's name, as added by Add character. Nobody talking, the camera does what
it is keyed to do. In Scene data it is the camera's `speaker`: `{"zoom": 1.7, "window": 0.5, "quiet": 0.06}`.

## Layout and brand workflow

The composer toolbar provides **Snap**, **Arrange**, **Lock**, **Unlock** and **Brand kit…**.

- Select several layers in the list, or Shift-click on the canvas. Moves keep the selection together. Snap uses canvas edges/centre, 5% safe margins and other visible layer edges/centres; orange guides show the match. Hold Alt to bypass snapping. Focus the canvas and use arrow keys to nudge, or Shift+arrows for ten preview pixels.
- Arrange against **Canvas**, **Safe margins** or **Selection**. Align left/right/top/bottom or either centre; distribute three or more layers with equal gaps. Selection distribution retains the outside bounds. Transformed parents and the scene camera are accounted for. Select a group or its children, not both.
- Layout operations offset animated positions while retaining their curves. With **Record keyframes** enabled, canvas moves and arranging instead record positions at the playhead.
- Locks are saved in the scene and protect canvas manipulation, property controls and layout operations. Locking a group also protects descendants. Locked layers remain selectable in the list for unlocking. Full advanced scene replacement remains an explicit way to change any scene data.
- Assign a **Brand colour** role in Layout, then apply a kit's primary, secondary, accent, background or foreground colour. A kit also supplies static text fonts. Animated styles and locked layers are preserved. Shared symbol definitions used by locked instances are preserved too.
- Open/save kits as JSON under your profile's `Media/Graphics Templates/Brands` folder. Ordinary scene templates retain the chosen kit, role tags, animation and locks, so layouts can be reused across projects.

MCP `graphics_overlay` adds `layout`, `lock` and `brand` actions. `get` returns stable addressable layer IDs, including prepared legacy layouts. Layout takes `layer_ids`, `operation` (`left`, `hcenter`, `right`, `top`, `vcenter`, `bottom`, `distribute_h`, `distribute_v`), `reference` (`canvas`, `safe`, `selection`) and event-local `time`. Lock takes `layer_ids` and boolean `locked`. Brand takes `brand` with `version:1`, `name`, `font` and the five colour-role values. Each is one undoable project edit. Keyframe edits respect layer/group locks.

## Scene schema

```json
{
  "version": 2,
  "variables": {"headline":"A NEW CHAPTER"},
  "datasets": {"results":[{"label":"A","value":64},{"label":"B","value":88}]},
  "symbols": {},
  "audio": {},
  "camera": {"x":0.5,"y":0.5,"zoom":1,"rotation":0},
  "layers": [
    {"type":"rect","id":"panel","x":0.05,"y":0.12,"width":0.90,"height":0.72,"fill":"#12243b"},
    {"type":"text","id":"heading","x":0.08,"y":0.16,"width":0.84,"height":0.12,
     "text":"{{headline}}","font":"Segoe UI","font_size":0.055,"bold":true,"fill":"#ffffff",
     "text_animation":{"unit":"word","style":"rise","stagger":0.1,"duration":0.5}}
  ]
}
```

Layers are bottom-to-top. Types: `rect`, `ellipse`, `line`, `text`, `image`, `path`, `group`, `instance`, `repeater`, `particles`, `chart`. Stable IDs identify parents, paths, masks and camera targets. IDs beginning with `$` are reserved for MCP editing targets. A child uses fractions of its group's bounds; roots use canvas fractions. Font size and stroke width use canvas height. Version-2 rounded-corner radius uses the layer's height. Rotation is degrees. `anchor_x/anchor_y` default to 0.5; `scale_x/scale_y` default to 1. `in/out` control event-local visibility, `opacity` is 0–1. Optional `blend` is `add`, `screen` or `multiply`.

Groups can contain `children`, or children can identify a flat `parent` group. The composer presents nested groups as parented layers. A group's opacity applies to its composed children once, including overlaps.

## Keyframes and bindings

```json
{"x":{"keyframes":[[0,0.1,"ease_in_out"],[3,0.8]],"loop":3,"ping_pong":true}}
```

Time is event-local seconds. Each key is `[seconds,value,easing?]`; keys have strictly increasing, unique times. The first key's curve controls the segment to the next key; an animation-level `easing` is the fallback. Curves: `linear`, `hold`, `ease_in`, `ease_out`, `ease_in_out`, `smooth`, `bounce`, `elastic`, or cubic Bezier `[x1,y1,x2,y2]`. Bezier time controls must be 0–1; value controls can overshoot. `loop` is a forward duration, and `ping_pong` adds a reverse leg of that duration.

Colours are `#RRGGBB`, colour names, or `#AARRGGBB` with the alpha first (`#80000000` is half-transparent black; `#00000080` is fully clear). For see-through shapes, `opacity` is simpler. Numbers interpolate, colours interpolate RGBA, and text/fonts/booleans change on their key. Compatible arrays and objects interpolate recursively. Animate geometry, styling, camera settings, path progress, text, chart values, mask dimensions, effect settings and procedural parameters. Structural IDs, type, parent links, symbol identity and child structure are not keyframe properties.

`{"variable":"name"}` binds a property to a scene variable; an optional numeric `offset` shifts a numeric binding. Text substitutes `{{name}}`. Symbol defaults and instance overrides supply reusable variables; repeaters also supply `index`.

## Paths, morphing and following

Paths use normalized coordinates within their layer box:

```json
{"type":"path","id":"route","x":0.1,"y":0.2,"width":0.8,"height":0.6,
 "commands":[["M",0,0.8],["C",0.25,0,0.7,1,1,0.2]],
 "fill":"#00000000","stroke":"#53e3d0","stroke_width":0.01,
 "stroke_dash":[5,3],"trim_end":{"keyframes":[[0,0],[5,1]]}}
```

Commands: `M x y`, `L x y`, `Q cx cy x y`, `C c1x c1y c2x c2y x y`, `Z`. Morph `commands` with keyframes whose command types and point counts match. `trim_start/trim_end` reveal a stroke by its actual length. Dash lengths are multiples of pen width; `dash_offset` animates their offset.

```json
{"type":"instance","id":"car","symbol":"car_design","width":0.06,"height":0.05,
 "follow":{"path":"route","progress":{"keyframes":[[0,0,"linear"],[8,1]],"loop":8},
           "orient":true,"loop":true,"angle_offset":0}}
```

Following uses arc length, not Bezier parameter speed. Progress keys control acceleration/braking. `orient` rotates to the tangent; `angle_offset` adjusts the object's nose direction. `offset_x/offset_y` move relative to the path in the follower's parent coordinate space. Path references work across transformed groups in the same composition. Camera `follow` accepts any layer ID, with optional offsets; camera x/y, zoom and rotation are animatable.

## Masks, styling and motion

`mask` can name another shape/path layer, or be `{type:rect|ellipse|path,x,y,width,height,commands?,invert?}` relative to the masked layer's box. Masks also clip grouped contents. Animate mask geometry for wipes/reveals.

`gradient` supports `linear` or `radial`, with `stops:[[position,colour],...]`, normalized `x1/y1/x2/y2`, and radial `radius`. `shadow` and `glow` have `color`, `opacity`, `blur`, `x`, `y`; blur and offsets use canvas fractions. They follow the painted silhouette, including stroked paths.

`trail:{samples:8,duration:0.3,opacity:0.3}` draws fading past positions. `motion_blur:{samples:5,shutter:0.03}` averages samples around the frame time without reducing a stationary object's opacity. Effects are derived from time, so seeking and export match.

Text animation: `unit:character|word|line`, `style:fade|rise|typewriter|scale`, `delay`, `stagger`, `duration`, `easing`, optional `distance` and `loop`. Character animation respects Unicode grapheme boundaries. Normal text wraps within its box, with `align`, `font`, `font_size`, `bold`, `tracking` and `line_spacing`.

## Reusable objects, data and procedural graphics

`symbols:{name:{layers:[...],variables:{...}}}` stores reusable local compositions. `instance` supplies `symbol` and optional variable overrides. Definition IDs are local to their composition. Recursive symbol/parent/path dependencies are rejected.

`repeater` repeats its `children` or `symbol`, with `count`, `columns`, `spacing_x/spacing_y`, `rotation_step`, and `stagger` (a child-animation time offset per copy). Copies share a definition and receive a zero-based `index` variable.

`chart` takes `data:[{label,value,color?},...]` or a `dataset` name from scene `datasets`. Styles are `bar`, `line`, `donut`; `progress`, `maximum`, `font_size`, `text_color` and values can animate. JSON import accepts an array or `{data:[...]}`. CSV columns are label, value and optional colour; a header is optional, and quoted fields are supported. Chart values are nonnegative in the renderer.

`particles` uses deterministic `seed`, `count`, `lifetime`, `speed`, `gravity`, `direction`, `spread`, `size`, `spin`, `shape:rect|ellipse`, and `colors`. Continuous emission is the default; `emission:burst` and `delay` emit once. Speed, gravity and size use normalized units. Particle layout is reproducible without replaying earlier frames.

## Audio animation

Bind an imported audio/video source through the composer or MCP. Mutant Video Producer decodes actual stereo peak envelopes and runs its native beat tracker, storing `left/right`, duration, detected beats, BPM and confidence in scene `audio`. Detection quality depends on the source; it is not a manual transcription of musical beats. Analysis uses source audio before timeline effects and does not add an audio event.

```json
{"scale_y":{"audio":"source","base":0.8,"amount":0.6,"channel":"peak","attack":0.03,"release":0.12}}
```

Channels are `peak`, `left`, `right`, `beat`; numeric base and amount control response. Attack/release use seconds. Beat response decays from stored beat times. A binding's `rate/offset` maps local scene time to source-envelope time; source-event binding accounts for straight playback speed. Reverse, loops and speed ramps require a rendered source. Envelopes are snapshots; source changes require rebinding. Missing bindings use the base value. No audio file is reopened when drawing an embedded envelope.

## MCP actions

- `graphics_overlay {action:"capabilities"}` returns features and limits.
- `presets`, `create`, `get`, `update` support both scene versions. Updates replace the full scene.
- `validate` checks a supplied scene without changing the timeline.
- `keyframe` takes event ID, layer ID (or `$camera`/`$variables`), property path, event-local time, value and easing. It retains the original static value at time zero when adding a later first key.
- `bind_audio` takes event ID, optional binding name (default `source`), media/source-event ID, source range and bins. Alternatively reuse `waveform_event_id`; existing waveforms may not contain detected beats.
- Lip sync: `source_track` (a track index) binds every file-backed audio event on that track during the scene, each at its
  place on the timeline, so one binding per character's voice track drives that character's mouth across all their lines
  (e.g. bind `bolt` to Bolt's voice track and give the mouth `scale_y: {audio: "bolt", base: 0.1, amount: 1.2}`). Track
  envelopes have no beats. In Compose Graphics, Bind audio for animation lists the audio tracks too.
- `characters`, `add_character` (`character`, `name`, `x`, `ground`, `colours`) and `save_character` (`layer_id`,
  `name`) use the character library; `expression` (`layer_id`, `mood`, `time`) changes a character's face from `time`.
- `move` adds a ready-made move or gesture (`move`, `layer_id`, `time`, `duration`, `amount`, `to`).
- `sets` lists the set library; `add_set` (`event_id`, `set`: a name or file) puts one behind the scene.
- `patch` (`event_id`, `operations`: `[{layer_id, property, value}]`, or `remove: true` instead of a value) changes
  properties without sending the whole scene: `property` is a dotted path (`shadow.blur`), and `layer_id` can be a layer,
  `$camera`, `$variables` or `$scene` (the scene's own keys, such as `motion_blur`). Locked layers are refused.
- `dance` (`event_id`, `dancers`: layer IDs, optional `moves`, `every`, `from`, `to`, `amount`) finds the beat of the music
  under the scene and keys a move on each beat for every dancer, the moves in turn (default hop, spin, jump, squash, arms
  up, shake), as one undo step.
- `batch` (`event_id`, `operations`: `[{action, ...}]`) runs `move`, `expression`, `keyframe` and `patch` steps in order as
  one undo step; if any step fails, nothing changes and the reply names the step. Both answer briefly instead of with the
  scene.
- `export_template` takes event ID, path and explicit `overwrite` when replacing a file.
- `import_template` validates a JSON file and inserts a new editable event, with optional at/length/track.

## Limits and scope

Scenes: 4 MB, 1024 stored layers including definitions (about a dozen library characters and a set), depth 16, 256 keys per property, 512 commands per path and 4096 expanded reusable layers. Repeaters: 128 copies. Particles: 2048 per layer. Charts: 128 rows. Audio: one-hour analysis ranges, 64–8192 bins, at most 8192 detected beat times. Rendering has a bounded 32,768 drawing-operation budget; keep very large nested particle/text/effect compositions below it. Text animation processes up to 2048 units and text layout up to 256 lines.

This is a 2D scene graph with articulated parenting, not a full 3D renderer or an IK/character-rigging suite. The scene format and renderer are separated so additional animation systems can be added later.
