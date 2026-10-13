# 10. Speed, Slow Motion and Ramps

You learn: changing speed, reversing, frame blending, keeping the pitch, speed ramps and freeze frames.

**Example project:** [speed-and-time](../../data/examples/speed-and-time.mtg)

## What's in it

The sample clip (with its timecode in the picture) five times, named on the **Speeds** track:

- at **100%**,
- at **50%** (slow motion, twice as long),
- at **200%** (half as long),
- **reversed**,
- with the ramp **Slow Motion in the Middle**.

Watch the timecode in the picture: it shows which part of the clip is playing.

## 1. Compare them

Play the project. The title bars show `[50%]`, `[200%]`. Listen too: the music changes speed with
the picture, keeping its pitch.

## 2. Time-stretch with the mouse

1. Hold **Ctrl** and drag the right edge of the *100%* event to the right. The footage stays the
   same and plays slower to fill the new length.
2. Press **Ctrl+Z**.

## 3. Event Properties

1. Select the *50%* event and press **Ctrl+Shift+E**.
2. Set the **Playback rate** to 25%. With **Keep the same footage** ticked the event gets longer;
   unticked, it keeps its length and shows less of the clip.
3. Tick **Blend frames (smoother slow motion)**.
4. Untick **Preserve pitch when the speed changes**: the sound now drops in pitch like a slowed tape.

## 4. Edit a ramp

1. Click the ramped event. Its velocity line shows on the event.
2. Drag the lowest point down to 10%: the slow part gets slower.
3. Double-click the line near the end to add a point, and drag it up to 300%.
4. Right-click a point > **Hold**: the speed jumps instead of gliding.
5. Drag a point below 0%: that part plays backwards.

## 5. Ready-made ramps

1. Select the *100%* event.
2. Right-click > **Speed Ramp** > **Freeze in the Middle**. Its sound is silent while frozen.
3. Try **Speed Up (100% to 300%)**.

## 6. A freeze frame

1. Put the cursor in the middle of the *200%* event.
2. Press **Shift+F**: the picture holds for 2 seconds there, and the rest moves later.
3. Trim the held frame to change how long it lasts.

## Next

- [Media, Proxies and YouTube](11-media-proxies-youtube.md)
- [Speed and Time](../speed.md) in the guide.
