# 2. Titles and Generators

You learn: titles and their styles, animating text in and out, kinetic text, credits, animated
backgrounds, overlays, shapes, and keyframing a title.

**Example project:** [titles-and-generators](../../data/examples/titles-and-generators.mvp)

## What's in it

36 seconds on four video tracks, everything drawn by Mutant Video Producer:

- **Backgrounds**: a countdown leader, a gradient, bokeh lights, a retro grid and a starfield.
- **Text**: a title with a subtitle, kinetic text, a typewriter, a lower third, a zoom title, and a credits roll.
- **Overlays**: a heart that moves and spins (keyframed), and gold confetti.
- **Progress**: a progress bar along the bottom for the whole video.

## 1. Restyle a title

1. Click the *Titles and Generators* event (at 5 seconds). The **Properties** tab opens.
2. Change the **Font** and **Size**.
3. Set an **Outline width** of 4 and pick an **Outline color**.
4. Raise **Shadow opacity** and **Shadow blur** for a soft shadow.
5. Raise **Background box opacity** for a box behind the text, and round its corners with
   **Background box corner radius**.
6. On the preview, drag the text to move it; scroll over it to resize it.

## 2. Animate it in and out

1. Set **Animate in** to *Slide* and **In duration** to 0.8 s.
2. Set **Animate out** to *Zoom*.
3. Play the event. Try *Typewriter* as the in animation too.

## 3. Kinetic text

1. Click the *Every word pops on in time* event.
2. Change the **Text**, and set **Words per second** to 2.
3. Try each **Animation**: Pop, Slide up, Fade. Pick a **Newest word color**.

## 4. Credits

1. Click the credits event at the end.
2. Lines starting with `# ` are headings. Add a heading and a few names.
3. Drag the event's right edge to make it longer: the credits scroll slower.

## 5. Backgrounds

1. Click the *Retro grid* background and change its **Grid color** and **Speed**.
2. Open the **Generators** tab. Drag **Plasma (fire)** onto the Backgrounds track, over the gradient.
   Plasma and the gradient overlap, so they crossfade.
3. Press **Ctrl+Z** to undo it.

## 6. Keyframe a shape

The heart moves and spins because its **Position X** and **Rotation** have keyframes (their values are orange).

1. Click the heart event (at 19 seconds).
2. Put the cursor half-way through it. Click the **◇** next to **Position Y**, then drag the heart
   up on the preview. A keyframe is set at the cursor.
3. Play it: the heart now arcs up and back down.
4. Right-click **Position Y** > **Keyframe Curve** > **Smooth**.

## 7. Add your own

1. Put the cursor at 30 seconds and press **Ctrl+T**: a title appears.
2. From the Generators tab, drag **Letterbox 2.39:1** onto the Overlays track: cinema bars slide in.
3. Drag **Shape: arrow** onto the Overlays track and drag it on the preview to point at something.

## Next

- [Transitions](03-transitions.md)
- [Titles and Generators](../titles-and-generators.md) in the guide lists every setting.
