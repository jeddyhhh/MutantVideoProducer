# 15. Recording, Streaming and the Switcher

You learn: recording clips, the replay buffer, full screen and Pop Out, Spout and the virtual
camera, streaming, the switcher, layers, and MIDI.

**Example project:** [live-set](../../data/examples/live-set.mvp), started as in
[Performing Live](14-performing-live.md).

## 1. Record a clip

1. With the Live tab running, press **Rec**. It records the output, effects and overlays included.
2. Play some pads, then press **Rec** again to finish.
3. Go to the **Editor** tab: the clip is at the top of the **From Live** tab. Double-click it to
   place it on the timeline.
4. Tick **Clean picture** before recording to record the picture without the Live FX, and add
   effects later in the editor.

Clips are saved in `Videos\Mutant Video Producer Live`.

## 2. Keep the last minute

1. Set the **replay buffer** to 60 seconds.
2. When something great happens, press **Save Replay**: the last 60 seconds are saved as a clip,
   and the buffer carries on.

## 3. Show it

- **Full Screen** (or double-click the picture) puts the output on your second monitor or projector.
  **Esc** closes it.
- **Pop Out** moves the picture into its own window. Right-click it for an exact size (1080p,
  720p), **Always on Top**, or full screen. Close it to bring the picture back.

## 4. Send it to OBS, Zoom or Resolume

In **Setup > Send**:

- **Spout**: OBS (with the Spout2 plugin), Resolume, TouchDesigner and MadMapper see a source called
  *Mutant Video Producer*, straight from the graphics card.
- **Virtual camera**: Zoom, Discord, Teams and browsers see the *OBS Virtual Camera* webcam showing
  Mutant Video Producer's picture. OBS Studio must be installed, and its own virtual camera off.

Both keep going while you work on the Editor tab.

## 5. Go live

1. **Setup > Send > Stream Settings...**: choose the service (Twitch, YouTube, Facebook, Kick, or
   a custom RTMP / SRT server), paste your **stream key**, and set the bitrate.
2. Press **Go Live**. The show bar shows *LIVE on Twitch* and the time.
3. Press **End Stream** to finish.

The stream key is kept encrypted for your Windows account. Recording and the replay buffer work while
you stream.

## 6. The switcher

For a show with several cameras, screens or videos:

1. In **Setup > Switcher**, add up to four more inputs, for example a second camera and a screen.
   A tile for each appears under the picture.
2. Click a tile: it goes into **preview** (green).
3. Press **Enter** (**Cut**) to put it on air (red), or **Shift+Enter** (**Auto**) to switch with the
   pads' transition.
4. Double-click a tile to cut to it straight away.

Effects, pads, overlays, recording and streaming follow whatever is on air.

## 7. A layer

1. In **Setup > Layer**, choose a second source, for example your webcam.
2. Choose **Picture in picture**, the bottom-right corner, and 25% wide.
3. In the pads panel, set **Edit** to **Layer FX (the second source)** and drop **Remove Background
   (AI)** in: the webcam's background disappears, and you appear over the main picture.

## 8. A MIDI controller

1. Plug in a controller and choose it in **Setup > MIDI**.
2. Right-click a pad > **Learn MIDI...**, then press a pad on the controller. Repeat for the others.
3. Right-click **Tap**, **Set 1** or **Rec** > **Learn MIDI...** to put them on buttons too.
4. Right-click a Live FX knob > **Modulate...** > **MIDI controller**, and **Learn** a knob or fader.
5. If DJ software or a drum machine sends a MIDI clock, set **Auto** to **MIDI clock** to follow its tempo.

## Next

- [Letting an AI Assistant Edit](16-ai-assistant.md)
- [Live](../live.md) in the guide.
