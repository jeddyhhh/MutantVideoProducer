# 6. Glitch and Retro Looks

You learn: glitch effects, Broken Video Game Systems, analog looks, limiting a glitch to part of the
picture, and glitch presets.

**Example project:** [glitch-and-retro](../../data/examples/glitch-and-retro.mvp)

## What's in it

Nine 4-second looks on the sample clip, named on the **Looks** track: **VHS**, **CRT Monitor**,
**RGB Split**, **Pixel Sort**, **Datamosh**, a **NES** effect from Broken Video Game Systems,
**Old Film**, **Kaleidoscope**, and the preset chain **Broken Stream**.

## 1. Tweak the looks

1. Click the *VHS* event and raise its **Noise**, **Jitter** and **Colour bleed**.
2. Click the *Pixel Sort* event. Change **Sort by** to hue, and raise the **Lower threshold** to sort
   only the bright parts. Change **Direction** to sort columns instead of rows.
3. Click the *Datamosh* event. Raise **Motion** to push the melt further. Set **Auto clean every** to 3 s to
   restart from a clean frame every 3 seconds.

## 2. Limit a glitch

Every glitch effect has **Mix** and **Apply to**.

1. Click the *RGB Split* event.
2. Set **Apply to** to *Inside box*. Move and size the box with **Box left**, **Box top**, **Box
   width** and **Box height**.
3. Set **Apply to** to *Bright areas*: only the highlights split. **Mask threshold** sets how bright.
4. Lower **Mix** to blend it with the clean picture.

## 3. Broken Video Game Systems

1. Click the *NES* event.
2. In the Video FX tab, open **Broken Video Game Systems > Nintendo NES (Famicom)** and drag another
   NES glitch onto the event.
3. Change **Console Look** to leave out the console's resolution and colours and keep just the glitch.
4. Raise **Intensity**, and change the **Seed** for a different pattern.
5. Try a **Game Boy**, **PlayStation** or **ZX Spectrum** effect instead.

## 4. Presets

1. Expand **Pixel Sort** in the Video FX tab and drag **Melting Highlights** onto an event.
2. Open **Preset Chains > Glitch** and drag **Datamosh Trip** onto another.

## 5. Mix and match

1. Drag **CRT Monitor** onto the Video track's header: every event now looks as if it's on an old TV.
2. Drag **Strobe** onto the *Kaleidoscope* event, and keyframe its **Opacity** so it builds up.

Animated effects are worked out from the frame number, so every render is identical.

## Next

- [Mixing Audio](07-mixing-audio.md)
- [Video Effects](../video-effects.md#glitch) in the guide lists every setting.
