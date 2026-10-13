# 14. Performing Live

You learn: choosing a source, pads, transitions, keys, momentary pads, tempo and auto switch,
launching on the beat, scenes, the setlist, modulation, overlays and random chains.

**Example project:** [live-set](../../data/examples/live-set.mtg)

## What's in it

The project's **Live tab** is set up for a performance:

- **Pads** (keys in brackets):
  - *Neon* (1), *Kaleido* (2), *Mirror* (3), *Psychedelic* (4),
  - *VHS* (Q), *CRT* (W), *Trails* (E), *Mono* (R),
  - four random chains, one of each strength from Low to Insane (A, S, D, F),
  - two **momentary** pads, on only while held: *Strobe* (Z) and *RGB Burst* (X).
- A soft **Glow** as the Live FX, under every pad.
- **Overlays**: *MVP LIVE* at the top, pulsing on the beat, and a spectrum along the bottom.
- **Scenes**: *Intro* (F1) and *Drop* (F2).
- A **setlist** of two songs: *Warm-up* at 120 BPM and *Peak* at 128 BPM.

## 1. Choose a source and start

1. Open the example, then click the **Live** tab.
2. In **Setup > Input**, set the **Source** to **Choose video files...** and pick
   `Documents\Mutant Video Producer Examples\media\sample.mp4`. (Or choose your webcam, or a screen.)
3. Set **Audio** to *Desktop audio (default output)*, so the music your computer plays drives the beat.
4. Press **Start** (or **Space**).

## 2. Play the pads

1. Click *Neon*: it switches on. Click it again: off.
2. Press **1**, **2**, **3**, **4** on the keyboard. Each switches with the chosen **Transition**.
3. Change the Transition to *Cross Zoom* and its length to 1 second, and switch again.
4. Hold **Z** for the strobe, and **X** for an RGB burst, on top of whatever pad is on.
5. With **Follow** on, the device chain shows the pad you switched on: turn its knobs while it plays.

## 3. Tempo and auto switch

1. Play some music on the computer.
2. Turn on **Auto** and choose **Audio beats**: the light flashes on each beat it hears, and the BPM shows.
3. Set it to switch every 4 beats, and tick **Random**: the pads change by themselves.
4. When the tempo is right, click **Lock BPM**.
5. Press **B** on the first beat of a bar (**Set 1**): switches now land on bars, and the light
   flashes orange on each 1.
6. No music? Turn off Audio beats, type 120 in the BPM box, or press **Tap** in time.

## 4. Launch on the beat

1. Set **Launch** to *On the next bar (the 1)*.
2. Press a pad: its outline is dashed until the next bar, then it switches, in time with the music.

## 5. Scenes and the setlist

1. Press **F2** (*Drop*): the Psychedelic pad and both overlays come back as saved.
2. Change something, then right-click scene 3 > **Save the Current Look Here**.
3. Press **Page Down**: the setlist moves to the next song, *Peak*. Its tempo (128, locked), its
   scene and its note come with it.
4. Click **Edit...** under the setlist to change the songs.

## 6. Modulation

1. Click **Edit** > *Kaleido* (the pad's chain shows below).
2. Right-click a knob > **Modulate...**, choose **Beat pulse**, and an **Amount** of 0.5.
3. Switch on *Kaleido*: that setting jumps on every beat. The knob has a dotted ring.
4. Try an **LFO** over 4 beats, or **Bass** from the audio input.

## 7. Overlays

1. Click **Overlays...**.
2. Change the text, or add a **Picture** (your logo) in a corner.
3. Change the spectrum to a **Ring** in the centre.

## 8. Random chains

1. Right-click an empty pad (**C** or **V**, at the end of the bottom row) > **Create Random Effects Chain > Insane**.
2. Play it. Don't like it? **Ctrl+Z**, and try again.
3. **≡ > Fill Empty Pads with Random Chains** fills every empty pad at once.
4. **≡ > Save As...** keeps the whole grid as a pad set, for any project.

## Next

- [Recording, Streaming and the Switcher](15-recording-streaming-switcher.md)
- [Live](../live.md) in the guide.
