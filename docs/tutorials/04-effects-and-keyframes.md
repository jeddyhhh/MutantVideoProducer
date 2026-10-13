# 4. Effects and Keyframes

You learn: the device chain, keyframes and their curves, racks, track and output effects, presets,
masks and Track Motion.

**Example project:** [effects-and-keyframes](../../data/examples/effects-and-keyframes.mtg)

## What's in it

The sample clip, a yellow ball moving over a retro grid, twice:

- The **first event** has two event effects: a **Gaussian Blur** that clears over the first 2
  seconds (keyframed), and a **rack** with two chains side by side, *Mono* (Black and White) and
  *Glow*, screened together.
- The **second event** has a **Pixelate** inside an ellipse **mask**, sitting where the ball starts.
- The **Video track** has a **Film Grain** effect (track FX).
- The **output** has a **Vignette** (output FX).

## 1. The device chain

1. Click the first event. The **Effects** tab at the bottom shows its chain: the blur and the rack.
2. Switch the **Event / Track / Output** tabs above the chain. *Track* shows the Film Grain,
   *Output* the Vignette.
3. Back on *Event*, click the blur's on / off button to switch it off and on.

## 2. Keyframes

The blur's **Horizontal** and **Vertical** values are orange: they have keyframes.

1. Move the cursor across the first 2 seconds and watch the values change.
2. Right-click **Horizontal** > **Next Keyframe** to jump to the next one.
3. Right-click > **Keyframe Curve** > **Smooth**: the blur now clears gently.
4. Put the cursor at 6 seconds and click the **◇** next to the Glow's **Intensity**. Move to
   10 seconds and turn the knob down. The glow now fades out between those two points.

## 3. The rack

1. Click the rack. Its chain list shows *Mono* and *Glow*.
2. Click *Glow* to see its devices. Change the chain's blend mode from *Screen* to *Add*.
3. Solo *Mono*: only that chain shows. Un-solo it.
4. Turn the rack's **Dry/Wet** down to mix the original back in.
5. Drag **Vignette** from the Video FX tab onto the rack's chain list: a third chain starts.

## 4. Presets

1. In the Video FX tab, expand **Curves** and drag **Faded Film** onto the first event.
2. Click the **≡** button on the new device and choose another preset.
3. Choose **≡ > Save as Preset...** to keep your own settings, and **Save Chain** (above the chain) to keep the whole chain.

## 5. Masks and Track Motion

1. Click the second event. The Pixelate device has its mask on: the ellipse shows on the preview.
2. Put the cursor at the event's start (12.5 seconds).
3. Click **Track Motion** on the Pixelate device. Mutant Video Producer follows the ball to the end of the event
   and keyframes the mask on every frame.
4. Play it: the pixelation stays on the ball.
5. Tick **Outside the mask**: now everything except the ball is pixelated.
6. Set the mask's **Feather** for a softer edge.

## 6. Follow Audio

1. Click the first event, right-click the Glow's **Intensity** > **Follow Audio...**.
2. Set **Listen to** to *Bass*, **When quiet** to 0 and **When loudest** to 2.
3. Play: the glow now pumps with the kick drum.

## Next

- [Colour Grading and Scopes](05-colour-grading.md)
- [Masks, Stabilising and Removing Backgrounds](12-masks-stabilize-background.md) goes further with masks.
- [Effects](../effects.md) in the guide.
