# Graphics and waveforms via MCP

The graphics tool now also supports general animation scenes. See [Animation scenes](animation-scenes.md)
for scene version 2, keyframe/audio actions, paths, groups, cameras, templates, data and procedural layers.
Version-1 scenes below remain supported and keep their original appearance.

`graphics_overlay` and `waveform_graphic` create regular generated timeline events. Times are seconds.
Creation returns `event_id`; use `edit_event` for timeline moves/fades/deletion and `undo` as usual.
`get_project` exposes `generator_data`, and each tool's `get` action returns its complete saved data.

## Compositions

`graphics_overlay` actions: `presets`, `create`, `get`, `update`. `presets` returns every starting scene.
Choose `blank`, `broadcast`, `lower_third`, `info_card` or `timeline`. A custom `scene` replaces the preset.
`update` replaces the complete scene; read it first if changing only one layer. Generator `params`
currently supports `opacity`, including normal Mutant Video Producer keyframes.

```json
{"action":"create","at":0,"length":20,"scene":{"version":1,"layers":[
  {"type":"rect","name":"Panel","x":0.05,"y":0.75,"width":0.9,"height":0.16,"fill":"#12243b"},
  {"type":"text","name":"Headline","x":0.07,"y":0.76,"width":0.85,"height":0.1,
   "text":"THE STORY BEHIND THE SOUND","font":"Segoe UI","font_size":0.04,"bold":true,"fill":"#ffffff"}
]}}
```

Layers render bottom to top. Supported `type`s: `rect`, `ellipse`, `line`, `text`, `image`.
Common properties: `name`, `visible`, `x`, `y`, `width`, `height`, `fill`, `stroke`, `stroke_width`,
`opacity`, `rotation`, `in`, `out`. Geometry, radii, stroke width and font size use fractions of the
canvas, rotation uses degrees, opacity is 0–1. `in`/`out` are event-local visibility times.
Text adds `text`, `font`, `font_size`, `bold`, `align` (`left`, `center`, `right`). Image adds `path`.
Rectangles add `radius`. Text wraps and clips inside its box; images fit without distortion.

Individual numeric geometry, size, rotation and opacity can use
`{"keyframes":[[0,0.1],[2,0.4]]}` with sorted event-local seconds and linear interpolation.
Use no more than 128 layers, 256 keys per property, or 2 MB of scene data. Invalid updates are rejected
before changing the timeline. Image paths are file references; shapes and text need no external assets.

## Real audio waveforms

`waveform_graphic` actions: `create`, `get`, `update`, `refresh`.

```json
{"action":"create","source_event_id":123,"params":{"color":"#66dddf",
 "played_color":"#ffffff","y":0.72,"height":0.18,"style":"Bars","stereo":true}}
```

`source_event_id` supplies the media, source in-point, span, timeline position, duration and playback
rate from a straight file-backed event with audio. Reversed clips, loops and speed ramps must first
be rendered to a file. Alternatively use `media_id`, `source_in`, `duration`, `at` and `length`.
`duration` is source time; `length` is timeline time. `bins` controls analysis detail (64–8192, default
2048). `track` can select a preferred video track. Analysis stores a snapshot of pre-effect source
audio, not a live link to the audio mixer; it does not insert or modify audio events.

`update` with only `params` changes appearance without decoding again. Supplying source/range fields
rebuilds peaks. `refresh` rebuilds the stored source range from its imported media. Reimport a removed
source before refreshing. Source clips changing later do not silently rewrite the waveform.

Settings: `x`, `y`, `width`, `height`, `color`, `played_color`, `background`, `background_opacity`,
`style` (`Bars`, `Filled envelope`, `Line`), `stereo`, `normalize`, `amplitude`, `thickness`, `spacing`,
`progress`, `scroll`, `window` (seconds), `rate`, `offset` (playhead seconds). Numeric settings support
normal Mutant Video Producer `{keyframes:[[seconds,value],...]}` animation. Peaks are embedded, so preview and
export never need to reopen the audio file to draw them.
