# Editing on the Timeline

## Tracks and events

The timeline has **video tracks** on top and **audio tracks** below. A video track higher up is
drawn over the ones below it. A file or generated picture placed on a track is an **event**.

### Tracks

- **Add** a track: **Insert > Video Track** (**Ctrl+Shift+Q**) or **Insert > Audio Track** (**Ctrl+Q**).
- **M** and **S** in a track header mute and solo it.
- **Level / Vol** and **Pan**: drag the bars in the header. Double-click a bar to reset it.
- **Rename**: double-click the track name.
- **Resize**: drag the bottom edge of the header.
- **Reorder**: drag a header up or down, or right-click > **Move Track Up / Down**.
- **Lock**: right-click a header > **Lock Track**. A padlock shows in the header and the events
  are hatched. They can't be selected, moved, trimmed or dropped onto, Split at the cursor leaves
  them whole, and ripple edits leave them in place. Mute, solo, level and pan still work.
- **Compositing mode** (video tracks): the **Mode** in the header, or right-click > **Compositing
  Mode**, sets how the track mixes with the tracks below it: Normal, Add, Screen, Multiply,
  Overlay, Lighten, Darken or Difference, at the track's level.
- **Track FX**: click a header, or its **fx** button, to show the track's effects in the device
  chain (see [Effects](effects.md)).
- **Meters**: each audio track's header shows its level while playing.

### Selecting

- Click an event to select it. **Ctrl+click** adds an event to the selection or takes it out.
- Drag a box from empty track space to select every event it touches. **Ctrl+drag** adds them to
  the selection.
- **Ctrl+A** selects everything.

### Moving and copying

- Drag an event to move it, also onto another track of the same kind.
- **Ctrl+drag** copies it (and its grouped audio). The copies are selected.
- **Alt+Left / Alt+Right** nudge the selection one frame. Hold **Shift** too for 10 frames.
- **Ctrl+X / Ctrl+C / Ctrl+V** cut, copy and paste. Paste goes in at the cursor.

### Trimming and splitting

- Drag an event's **left or right edge** to trim it.
- **S** splits at the cursor.
- **Alt+[** and **Alt+]** trim the selected events' start or end to the cursor.
- **Del** deletes. **Ctrl+Del** ripple-deletes: what follows moves back to fill the gap.
- **Loop past the end**: drag an event's end beyond the end of its file, and the file plays again
  from its start, picture and sound. A notch marks where each pass begins.

### Slip, slide and roll

- **Slip** (**Alt+drag** an event): the event stays where it is but shows an earlier or later part
  of its file, as far as the file goes.
- **Slide** (**Ctrl+Alt+drag** an event): the event moves between its neighbours. The event before
  it ends where it now starts and the event after it starts where it now ends.
- **Roll** (**Ctrl+Alt+drag the cut** between two touching events): one event gets longer and the
  other shorter, their audio with them. Nothing else moves.

### Fades and level

- Drag the small **handle in a top corner** of an event to fade it in or out.
- Right-click an event > **Fade In Curve** / **Fade Out Curve**: Linear, Smooth (S-curve), Fast,
  Slow, or Constant Power (keeps the loudness even through an audio crossfade). The fade is drawn in
  its shape. Video fades, crossfades and transitions follow the curve too.
- Drag the **line under the title bar** to change the event's level (opacity for video, volume for audio).

### Crossfades

Overlap two events on the same track and the overlap becomes a **crossfade**. Drop a transition on
it to change how it looks (see [Transitions](transitions.md)).

**Options > Automatic Crossfades** is on by default. With it off, an event moved, copied, trimmed
or placed over another **cuts** it instead: it shortens it, splits it around itself, or replaces it.

### Groups

- A video file's picture and sound are placed as a group: they move, trim and split together, and a
  transition on the picture fades the sound with it.
- **U** (right-click > **Ungroup Video and Audio**) separates them. **G** groups the selected events again.

### Muting and properties

- **Edit > Mute / Unmute Event** mutes the selected events.
- **Event Properties** (**Ctrl+Shift+E**) sets speed, reverse, frame blending and pitch (see
  [Speed and Time](speed.md)).
- Right-click an audio event > **Normalize** sets its peak to -0.1 dBFS (it adds a Gain effect).

## Auto ripple

With **auto ripple** on (**Ctrl+L**, or the **Ripple** button on the transport bar), edits push or
pull what comes after them:

- trimming an event's end or start,
- moving an event,
- deleting,
- pasting (which inserts and splits what is under the cursor),
- dropping media or titles into the middle of the timeline.

**Options > Auto Ripple** sets what moves:

- **Affected Tracks**: the later events on the tracks being edited.
- **All Tracks, Markers and Regions**: everything after the edit.

**Ctrl+Del** ripple-deletes even with auto ripple off.

## The time selection and loop region

- Drag in the thin bar above the ruler, or **Shift+drag** in empty track space, to select a stretch of time.
- Double-click an event to select its span.
- **I** and **O** set the start and end at the cursor, or where playback is.
- **Q** (**Options > Loop Playback**) loops playback inside the selection.
- [Render As](rendering.md) can render just the selection.

## Markers and regions

- **M** drops a numbered marker at the cursor, or where playback is. Tap it in time with the music
  while it plays.
- **R** turns the time selection into a **region**.
- Markers and regions live in the marker bar above the ruler:
  - Click one to go there. Drag it to move it. Drag a region's edges to resize it.
  - Double-click a marker to name it. Double-click a region to select its time (to loop or render it).
  - Right-click for colour, rename and delete.
- **[** and **]** jump to the previous and next marker.
- Events snap to markers.
- Markers are saved with the project and are undoable.
- [Detect Beats](audio.md#detect-beats) puts a marker on every beat or bar.

## Snapping and the grid

- **F8** (**Options > Enable Snapping**) turns snapping on and off. Hold **Shift** while dragging to
  bypass it.
- Events snap to other events' edges, markers, the cursor and the grid.
- **Options > Grid**: Off, Frames, Half Seconds, Seconds, or **Beats** (bars brighter). The grid is
  drawn on the tracks; events, edges and the cursor snap to it.
- **Options > Grid > Beat Grid Tempo...** sets the tempo and where a beat falls.
  **Tools > Detect Beats** sets them from the music.

## Envelopes

Envelopes change a track's volume, pan or video level over time.

- Select a track and press **V** for volume (on a video track: the composite level) or **P** for pan.
  Or right-click the track header.
- Double-click the line to add a point. Drag points (they snap), or drag a segment up or down.
- Right-click a point for **Linear**, **Smooth** or **Hold** curves, or to delete it.
- Right-click the line to reset or hide it.
- Envelopes follow timeline time. They multiply the track's own Level / Vol; pan is added to the Pan slider.
- **Shift+V** puts a speed ramp on the selected events (see [Speed and Time](speed.md#speed-ramps)).

## Playback and moving around

| Key | What it does |
|---|---|
| Space | Play / stop. Stopping goes back to where you started. |
| Enter | Play / pause where you are. |
| Shift+Space | Play from the start. |
| J / K / L | Shuttle backwards / stop / forwards. Press J or L again for 2x, 4x, 8x. |
| ← / → | Previous / next frame. |
| Ctrl+← / Ctrl+→ | Previous / next edit point. |
| Home / End | Start / end. |
| [ / ] | Previous / next marker. |
| ↑ / ↓, or mouse wheel over the time ruler | Zoom in / out. |
| Mouse wheel | Scroll the tracks up and down. |
| Ctrl+0 | Zoom to fit. |
| Ctrl+wheel or Shift+wheel | Scroll sideways. |

- Shuttling faster than 1x, or backwards, has no sound.
- **Clicking the timeline while it plays** jumps there and keeps playing. With **Options > Click
  While Playing Jumps There** off, playback carries on and the click only sets the new start point:
  press **Space** to stop there, and Space again to play from it.
- **Audio scrubbing**: dragging the cursor on the ruler or stepping frames plays a snippet of the
  sound there (**Options > Audio Scrubbing**).

## Undo and History

- **Ctrl+Z** undoes and **Ctrl+Y** redoes.
- The **History** tab (beside From Live) lists every step. Click one to go back, or forward, to it.
