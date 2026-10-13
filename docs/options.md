# Options

Everything in the **Options** menu. Settings are remembered between sessions.

## Themes

**View > Theme** changes how Mutant Video Producer looks, at once, and remembers it:

| Theme | Look |
|---|---|
| Classic | Neutral dark grey with blue accents (the original look). |
| Neon Grid | Cyberpunk: violet-black with hot magenta and electric cyan. |
| Night City | Cyberpunk: near-black with acid yellow, red and cyan, like a heads-up display. |
| Synthwave Sunset | Deep purple with orange and pink, like an '80s poster. |
| Terminal | Black with green phosphor text and amber highlights. |
| Arctic | Cool blue-grey with frosty accents: calm and low-contrast for long sessions. |
| Studio Light | A light theme for daytime and bright rooms. |

A theme colours the window, the timeline (tracks, events, the cursor and playhead, meters), the
device chain and knobs, the Live tab's pads and scenes, and the help window. Your footage,
thumbnails and effect previews are never tinted, and tooltips stay black on pale yellow.

## AI and Live

| Option | What it does |
|---|---|
| Allow AI Control (MCP) | Lets an AI assistant on this computer edit (see [AI Control](ai-control.md)). Off by default. |
| AI Control Setup... | The lines to paste into Claude Code, Claude Desktop or another MCP client. |
| Live Tab Layout | Sidebar, Stage or Compact (see [Live](live.md#the-layout)). |

## Editing

| Option | Default | What it does |
|---|---|---|
| Enable Snapping (F8) | On | Events, edges and the cursor snap to edges, markers, the cursor and the grid. Hold **Shift** while dragging to bypass it. |
| Keyboard Shortcuts... | | Every command and its key (see [Keyboard Shortcuts](shortcuts.md)). |
| Automatic Crossfades | On | Overlapping events crossfade. Off: an event moved, copied, trimmed or placed over another cuts it. |
| Grid | Off | Frames, Half Seconds, Seconds or Beats; **Beat Grid Tempo...** sets the tempo and a beat. |
| Loop Playback (Q) | Off | Plays the time selection over and over. |
| Click While Playing Jumps There | On | A click on the timeline while it plays jumps there and carries on. Off: playback carries on and the click sets where Space stops and starts next. |
| Auto Ripple (Ctrl+L) | Off | Off, Affected Tracks, or All Tracks, Markers and Regions (see [Auto ripple](editing.md#auto-ripple)). |
| Autosave | Every 2 minutes | Off, or every 1, 2, 5 or 10 minutes (see [Autosave](media.md#autosave-and-recovery)). |
| Audio Scrubbing | On | Hear the sound under the cursor while dragging it or stepping frames. |
| Reopen Last Project on Start | On | Opens the last project when Mutant Video Producer starts. |
| Check for Updates on Start | On | Once a day, asks GitHub whether a newer Mutant Video Producer is out (see [Updates](getting-started.md#updates)). |

## Performance

| Option | Default | What it does |
|---|---|---|
| GPU Acceleration (OpenGL rendering) | On | The preview and renders are drawn with OpenGL 3.3: colour conversion, scaling, Pan/Crop, blending, effects and transitions run as shaders. The FFmpeg Filter effect and text drawing run on the CPU. The result is the same either way. |
| Hardware Video Decoding | On | Decodes on the graphics card: D3D11VA on Windows, VAAPI or CUDA on Linux, for H.264, HEVC (and 10-bit), VP9, AV1 and others the card supports. Anything else decodes on the CPU. |

The status bar shows what is in use, for example
`Render: GPU (NVIDIA GeForce RTX 5070) Decode: D3D11VA`.

Hardware encoders (NVIDIA NVENC, AMD AMF, Intel Quick Sync) show up as templates in
[Render As](rendering.md) when this computer can use them.

## Elsewhere

- **Help > System Check...**: what this computer can do (see [Checking your computer](getting-started.md#checking-your-computer)).
- The welcome window: **Show this window when Mutant Video Producer starts** (see [The welcome window](getting-started.md#the-welcome-window)).
- **Tools > Proxy Size** and **Tools > Make Proxies on Import**: see [Proxies](media.md#proxies).
- **File > Properties** (**Alt+Enter**): the project's frame size, frame rate and sample rate.
