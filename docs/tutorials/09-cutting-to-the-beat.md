# 9. Cutting to the Beat

You learn: Detect Beats, markers and regions, the beat grid, Split at Markers, Beat Montage, and
looping a section.

**Example project:** [cutting-to-the-beat](../../data/examples/cutting-to-the-beat.mvp)

## What's in it

- **Music**: 32 seconds of house music at 120 BPM.
- **Background**: one long plasma event.
- **Project Media**: the sample clip, in a *Clips* folder, rated 4 stars.

## 1. Find the beat

1. Press **Ctrl+B** (**Tools > Detect Beats**).
2. Set **Markers on** to *Every bar (4 beats)* and **Listen to** to *Everything*.
3. Click OK. A marker appears on every bar, numbered from the first beat. Detect Beats also sets
   the beat grid's tempo (120 BPM).

## 2. Markers

1. Press **]** and **[** to jump from marker to marker.
2. Double-click a marker and name it *Drop*.
3. Right-click it > colour, and pick red.
4. Play, and press **M** in time with the music: you drop markers as it plays.
5. Press **Ctrl+Z** to take those away again.

## 3. The beat grid

1. **Options > Grid > Beats**. Beats are drawn on the tracks, bars brighter.
2. Drag an event's edge: it snaps to the beats.

## 4. Cut on the bars

1. Select the plasma event.
2. **Tools > Split Selected Events at Markers**: it is cut on every bar.
3. Drag different effects from the Video FX tab onto alternate pieces, for example **Invert** and
   **Kaleidoscope**: the look changes on every bar.

## 5. A beat montage

1. In Project Media, click the sample clip to select it.
2. **Tools > Make Beat Montage...**
3. **Cut**: *Every marker*. **Clips**: *Shuffled*.
4. Click OK. A new video track on top is filled with clips cut on every bar, each from a random part
   of the sample.
5. Run Detect Beats again with *Every beat* and make another montage for faster cuts.

## 6. Loop a section

1. Drag in the thin bar above the ruler from bar 5 to bar 9 to select that time.
2. Press **R**: it becomes a region. Double-click the region later to select it again.
3. Press **Q** and play: the section loops while you work on it.

## Next

- [Speed, Slow Motion and Ramps](10-speed-and-ramps.md)
- [Audio](../audio.md#detect-beats) and [Editing](../editing.md#markers-and-regions) in the guide.
