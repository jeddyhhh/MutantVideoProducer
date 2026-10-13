# 5. Colour Grading and Scopes

You learn: reading the scopes, Curves, Color Wheels, LUTs, the grading presets, and grading a whole
track or the whole video at once.

**Example project:** [colour-grading](../../data/examples/colour-grading.mtg)

## What's in it

- 3 seconds of **SMPTE colour bars**.
- The sample clip six times, 4 seconds each: the **original**, then the presets **S-Curve
  Contrast**, **Teal and Orange**, **Faded Film**, **Cool Moonlight** and the chain **Vintage
  Film**. The **Grades** track names each one.

## 1. The scopes

1. Click **Scopes** over the preview (**Ctrl+Alt+S**). A scope appears beside the picture.
2. Put the cursor on the colour bars. Choose **Vectorscope**: each bar lands on its target box.
3. Choose **Waveform**: the bars make steps at their brightness, 0-100%.
4. Choose **RGB Parade**, then **Histogram**.
5. Play the clip events and watch the scopes change with each grade.

## 2. Compare the grades

1. Put the cursor on *Original*, then on *Teal and Orange*. Watch the vectorscope: the colours lean
   towards teal and orange.
2. Click the *Teal and Orange* event. Its **Color Wheels** device shows the settings: Lift (shadows)
   pushed towards teal, Gain (highlights) towards orange.
3. Click the *Vintage Film* event: a chain of several devices. Switch each off and on to see what it adds.

## 3. Grade with Curves

1. Click the *Original* event.
2. Drag **Curves** from the Video FX tab (Color) onto it.
3. Lower **Shadows** and raise **Highlights**: an S-curve, more contrast. Watch the waveform spread out.
4. Raise **Shadows** instead: a faded, milky look.
5. Change only the blue **Shadows** for a cool tint in the dark parts.

## 4. Grade with Color Wheels

1. Drag **Color Wheels (Lift / Gamma / Gain)** onto the same event.
2. Push **Gain** towards orange and **Lift** towards blue.
3. Lower the Lift level a little for deeper blacks.

## 5. A LUT

If you have a `.cube` file (from a LUT pack, DaVinci Resolve, or your camera maker):

1. Drag **LUT (.cube)** onto an event and click **Browse** to choose the file.
2. Lower **Strength** to mix it with the original.

## 6. Grade everything at once

1. Click the Video track's header to show its **Track** chain.
2. Drag **Hue, Saturation and Lightness** onto it and lower the saturation: every event on the track changes.
3. Or choose **Edit > Video Output FX...** to grade the finished picture, titles included.

## 7. Grade part of the picture

1. Click the ◐ button on a grading device to give it a mask.
2. Set it to **Outside the mask** and put the mask over the ball: the grade now leaves the ball alone.

## Next

- [Glitch and Retro Looks](06-glitch-and-retro.md)
- [Video Effects](../video-effects.md#color) in the guide.
