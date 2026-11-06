# Video Effects

Every video effect in the **Video FX** tab, by folder. For how to add, keyframe, mask and save
effects, see [Effects](effects.md). Every number can be keyframed, and every effect can be
limited to part of the picture with a [mask](effects.md#masks).

## Color

| Effect | What it does |
|---|---|
| Brightness and Contrast | Raises or lowers brightness, and stretches or flattens contrast. |
| Levels | Remaps the input black and white points and gamma to an output range. |
| Hue, Saturation and Lightness | Rotates hues, boosts or reduces saturation, lightens or darkens. |
| Color Balance | Shifts red, green and blue separately in the shadows, midtones and highlights. |
| Curves | Raises or lowers the shadows, darks, midtones, lights and highlights (a smooth curve through them), then shapes red, green and blue separately. An S-curve adds contrast; lifting the shadows gives a faded film look. |
| Color Wheels (Lift / Gamma / Gain) | Grading in three ranges: Lift moves the shadows, Gamma the midtones, Gain the highlights. Each has a brightness and a colour push (a direction on the wheel and how far). |
| LUT (.cube) | Applies a 3D (or 1D) lookup table from a `.cube` file: DaVinci Resolve, Premiere, LUT packs, or a camera's log-to-Rec.709 LUT. **Browse** picks the file; **Strength** mixes it with the original. |
| Black and White | Greyscale. |
| Sepia | Old-photo brown toning. |
| Color Tint | Colours the picture with one tint. |
| Invert | Photographic negative. |
| Posterize | Reduces each channel to a few levels. |
| White Balance and Auto Colour | Each channel's black and white points, then a neutral colour made grey. Set by **Auto Colour** (right-click a video event, or **Tools > Auto Colour**), its **Auto** button, or **Pick Neutral**: then click something on the preview that should be grey or white. **Amount** tones it down. |

**Grading presets**: *S-Curve Contrast*, *Faded Film*, *Cross Process*, *Bright and Airy* (Curves);
*Teal and Orange*, *Warm Sunset*, *Cool Moonlight*, *Matrix Green* (Color Wheels); and the chains
*Blockbuster Grade* and *Vintage Film*. Curves, Color Wheels and LUT give the same result on the
GPU and the CPU.

## Blur & Sharpen

| Effect | What it does |
|---|---|
| Gaussian Blur | Smooth blur; horizontal and vertical can differ. |
| Sharpen | Unsharp mask: more local contrast around edges. |
| Glow | Bright areas bloom outwards. |

## Stylize

| Effect | What it does |
|---|---|
| Vignette | Darkens (or lightens) the edges of the frame. |
| Pixelate | A mosaic of large square blocks. |
| Film Grain | Random grain, optionally changing every frame. |
| ASCII Art | The picture as text characters, in colour or terminal green / amber. |
| Halftone | Printed dot screens: one ink, CMYK colour print, or RGB screen dots. |
| Neon Edges | Glowing outlines, in the picture's colours, a rainbow, or one colour. |
| Emboss | A raised relief from the picture's edges. |

## Keying & Compositing

| Effect | What it does |
|---|---|
| Chroma Keyer | Makes a colour (green or blue screen) transparent so lower tracks show through. Key colour, tolerance, softness, spill suppression, and *Show mask only* to check the key. |
| Transform (Picture in Picture) | Scales, moves, rotates and crops the picture, with an optional border. Keyframe it for animated picture-in-picture. |
| Mirror / Flip | Flips the picture horizontally and / or vertically. |
| Corner Pin | Puts the picture's four corners anywhere, in perspective: a video onto a phone, a TV or a sign. Drag the corners on its picture in the device chain. **Track Corners** follows them on the shot under the event (see [Effects](effects.md#track-corners)). |
| Remove Background (AI) | See below. |

### Remove Background (AI)

Finds the people in the picture and removes, blurs or recolours everything else, without a green
screen. It works on events, tracks and the Live tab.

- **Model**:
  - **Fast** (MediaPipe selfie segmentation): a quick person mask. **Threshold** and **Edge
    softness** shape its edge.
  - **Quality (video matting)** (RobustVideoMatting): a finer matte that follows hair and fine edges
    and stays steady on video by itself. Slower.
- **Background**: Transparent (tracks below show through), Blurred (**Background blur**), Color
  (**Background color**), or Show mask.
- **Steady (less flicker)**: smooths the mask over time.
- **Remove the person instead**: keeps the background and removes the people.

Both models run on the graphics card through DirectML (any DirectX 12 card), or on the CPU without
one. The blending runs in shaders on the GPU renderer.

## Advanced

**FFmpeg Filter (advanced)** runs any [FFmpeg filter chain](https://ffmpeg.org/ffmpeg-filters.html)
on the picture. Examples: `hqdn3d=4:3:6:4` (denoise), `eq=gamma=1.2:saturation=1.3`, `edgedetect`,
`unsharp=5:5:1.0`, `lenscorrection=k1=-0.2`. It always runs on the CPU.

## Glitch

Every glitch effect has a **Mix**, and an **Apply to** setting:

- **Everything**,
- **Inside box** or **Outside box** (position, size and softness can be keyframed),
- **Bright areas** or **Dark areas** (above or below the **Mask threshold**).

| Effect | What it does |
|---|---|
| Chromatic Aberration | Red and blue split apart like a cheap lens (radial, around any centre, with a falloff curve) or a misaligned signal (linear). Green can shift too; *Wobble* makes it breathe. |
| RGB Split | Shifts the red, green and blue channels independently. *Jitter* throws them around at random; edges stretch, wrap, mirror or go black. |
| Pixel Sort | Sorts runs of pixels whose brightness (or hue, or saturation) is between two thresholds, along rows or columns, by brightness, hue, saturation, R, G or B. Max run length and random breaks chop them into streaks. |
| Datamosh | Fakes deleted keyframes: the picture is pushed around by the footage's own motion, so images melt into each other. *Bloom* repeats the same motion, *Motion* pushes further, *Auto clean* restarts from a clean frame every few seconds. |
| Digital Corruption (Blocks) | Random blocks jump around, change colour or turn to static, like a corrupt stream. |
| Slice Displacement | Slices tear sideways (or up and down), with RGB fringes and colour glitches. |
| Compression Artifacts | Real DCT macroblocks of 8, 16 or 32 px: blocking, ringing and smeared colour. Corrupts any coefficient, only block colours (DC), or only detail. |
| Databend | Edits the raw colour bytes like audio: echo, reversed chunks, rotated bits, XOR, a byte shift, a wrong line length (stride), repeated chunks. |
| Bitcrush / Overflow | Fewer bits per colour (or separate R / G / B); bright colours wrap round to dark, or fold back. |
| Pixel Stretch | Smears one row or column towards the edge. Length, a sweeping line and per-line jitter. |
| Channel Swap | Rearranges the red, green and blue channels, and inverts any of them. |
| RGB Time Split | One channel shows now and the others lag a few frames behind: moving things leave colour ghosts. |
| Frame Rate Stutter | Plays at a lower frame rate; *Freeze* makes random frames stick. |
| Strobe | Flashes a colour (fixed, random or rainbow), inverts, blacks out, goes black and white, or hides the picture, with opacity and random skips. |

About 30 presets sit under the effects, for example Pixel Sort *Melting Highlights* and Databend
*Wrong Stride*. **Preset Chains > Glitch** has *Broken Stream*, *Datamosh Trip*, *Panic Attack*
and *Corrupted Tape*.

## Broken VGS

Broken Video Game Systems: a folder for each of 34 consoles and computers, with 128 glitches
modelled on how that machine's picture really breaks, and **Analogue Circuit Emu**, 37 effects made
in the Effect Builder that send the picture down an emulated composite video cable, through analogue
circuits, onto an old TV (see [Effect Builder](effects.md#effect-builder)). For example:

- **NES**: dirty cartridge, blinking lockout, scroll-split errors, sprite overflow flicker.
- **Game Boy**: vertical line death, Glitch City.
- **SNES**: Mode 7 meltdown, HDMA waves.
- **PlayStation**: wobbly polygons, cracks, exploding vertices.
- **Nintendo 64**: cartridge tilt.
- **Genesis**: jailbars, CRAM dots.
- **ZX Spectrum**: attribute clash, tape loading.
- **Commodore 64**: VIC-II sparkle. **Amiga**: Guru Meditation.
- **Xbox 360**: red-ring artifacts. **PS3**: YLOD checkerboards. **Vectrex**: collapsed deflection.

The consoles: Atari 2600, Atari Jaguar, Atari Lynx, ColecoVision, Commodore 64, Commodore Amiga,
IBM PC (CGA), 3DO, Microsoft Xbox, Xbox 360, NEC TurboGrafx-16, Nintendo NES, SNES, Nintendo 64,
GameCube, Wii, Switch, Game Boy, Game Boy Color, Game Boy Advance, Nintendo DS, Virtual Boy, SNK
Neo Geo, Sega Master System, Genesis, Game Gear, Saturn, Dreamcast, Sinclair ZX Spectrum, Sony
PlayStation, PlayStation 2, PlayStation 3, PSP and Vectrex.

Each effect redraws the picture at the console's resolution, with its colours (real palettes or
bit depths, console dithering) and screen (CRT scanlines or an LCD grid).

- **Console Look**: keep the console's whole look, or leave out its resolution, or leave out both
  its resolution and its colours, to keep just the glitch.
- **Intensity**, **Speed** and **Seed** tune the damage.

## Analog & Retro

| Effect | What it does |
|---|---|
| VHS | Soft picture, colour bleeding sideways, wobble, tape noise, dropouts and head-switching noise. |
| VHS Tracking Error | A band of torn, noisy picture rolling through the frame. |
| CRT Monitor | Curved glass, scanlines, a phosphor mask and dark corners. |
| NTSC Composite | Dot crawl, rainbow fringes on fine detail and blurred colour, from a real NTSC encode and decode. |
| TV Static | Snow, rolling hum bars and bands of lost signal. |
| Sync Wobble / Rolling | Lines wobble sideways and the picture rolls vertically. |
| Interlacing | Combing on motion, shifted fields or doubled lines. |
| Old Film | Scratches, dust, flicker, gate weave, grain, sepia and vignetting. |
| Ghosting | Faint repeated copies to the side, like multipath reception. |
| Dither / Retro Palette | The picture in a limited palette (56: computers, consoles, pixel art, colour depths, or colours picked from the picture) with one of 28 ways of dithering. See below. |

### Dither / Retro Palette

Reduces the picture to a palette, the way old computers, consoles and printers had to, and hides
the steps with a pattern of dots.

- **Palette**:
  - *PC*: CGA (each of its four-colour modes), EGA (16, and all 64), Windows (16 and 20),
    Macintosh II, RISC OS.
  - *Home computers*: Commodore 64 (three versions of its colours), ZX Spectrum, Amstrad CPC (27),
    MSX / ColecoVision, Apple II (hi-res), Amiga Workbench 1.3 and 2.0, Teletext / BBC.
  - *Consoles and handhelds*: NES, Game Boy (original, Pocket, and four popular tints), Virtual Boy,
    Nokia 3310, PICO-8, Sweetie 16 (TIC-80).
  - *Colour depths*: Web-safe (216), 9-bit (512: Atari ST, Mega Drive), 12-bit (4,096: Amiga, Game
    Gear), 15-bit (32,768: SNES, Game Boy Advance).
  - *Pixel art*: DawnBringer 16 and 32, Endesga 32, Vaporwave.
  - *Screens, print and moods*: black & white, 4, 8 or 16 greys, sepia, green and amber monitor
    screens, Risograph, CMYK print, Neon night.
  - *Adaptive*: 2, 4, 8, 16, 32 or 64 colours picked from the picture itself (each frame).
- **Dithering**:
  - *None*: each pixel the nearest colour (flat areas, banding).
  - *Ordered*: Bayer 2x2, 4x4, 8x8 and 16x16 (the classic cross-hatched look); clustered dots 4x4
    and 8x8 and a 45° halftone (like newspaper print); horizontal, vertical and diagonal lines and
    crosshatch (engraving-like).
  - *Noise*: blue noise (even, with no visible pattern: the most natural look), white noise (grainy)
    and interleaved gradient noise.
  - *Pattern (Knoll)*: mixes several palette colours in a fixed pattern; good with small palettes.
  - *Error diffusion*: Floyd-Steinberg, false Floyd-Steinberg, Jarvis-Judice-Ninke, Stucki,
    Burkes, Sierra, Sierra two-row, Sierra Lite, Atkinson (the original Macintosh look: higher
    contrast), Shiau-Fan and Shiau-Fan 2. Each passes what one pixel gets wrong to the pixels after
    it, so details and gradients come through.
  - *Riemersma*: error diffusion along a Hilbert curve, so no direction shows (this one is worked
    out on the CPU even with GPU rendering on).
- **Pixel size**: how big each dot is.
- **Strength**: how much dithering (0 = none, 1 = normal, 2 = double).
- **Match colours**: by weighted RGB (the classic way), or *perceptually* (OKLab), which keeps
  hues truer with odd palettes. (Error diffusion always matches by RGB.)
- **Animate noise**: the noise patterns move every frame, for a living, film-like grain.

## Psychedelic & Distort

| Effect | What it does |
|---|---|
| Kaleidoscope | Mirrored wedges around a centre; *Spin* animates it. |
| Mirror Symmetry | Reflects one half (or quarter) onto the other. |
| Wave Warp | Sine-wave distortion that can flow over time. |
| Twirl | Swirls the picture round a centre. |
| Ripple | Circular ripples from a point, like a drop in water. |
| Lens Distortion / Fisheye | Barrel (fisheye) or pincushion distortion. |
| Polar Coordinates | Wraps the picture into a circle, or unwraps a circle into a strip. |
| Tile / Repeat | Repeats the picture in a grid, optionally mirrored and scrolling. |
| Zoom / Spin Blur | Blurs towards or round a centre. |
| Melt | Bright parts drip and run in waves. |
| Camera Shake | Smooth random handheld shake, with a little zoom to hide the edges. |
| Trails / Echo | Each frame leaves a fading trail: light painting, motion echo, neon or dark trails. |
| Video Feedback | The previous output, zoomed, rotated and hue-shifted, under the picture: infinite tunnels. |
| Slit Scan / Time Displacement | Each line comes from a different moment: moving things stretch through time. |
| Psychedelic Colors | Cycling rainbow hues that shift with brightness and position. |
| Thermal / Gradient Map | Maps brightness to a gradient: thermal camera, night vision, infrared, acid. |
| Solarize | Inverts the tones above a threshold. |

Animated effects are seeded from the frame number, so every render is identical. The effects that
build on earlier frames (trails, feedback, datamosh, slit scan) start over after a jump in time.

## Retro Shaders (screen shaders)

The screen shaders RetroArch uses (libretro's GLSL collection): CRT screens with curvature, scanlines
and shadow masks, arcade and consumer TVs, handheld LCDs (Game Boy, GBA, DS, PSP), NTSC and VHS
signal looks, and the pixel-art upscalers (xBR, ScaleFX, HQx...), over 600 presets in all.

- **Tools > Get Screen Shaders (RetroArch's)...** downloads them from GitHub (about 30 MB) into the
  app data folder. They are not part of Mutant Video Producer: each is under its own licence (GPL,
  MIT, public domain...), written at the top of its file. Run it again to get the newest ones.
- Then they are effects under **Video FX > Retro Shaders**, in the same folders as RetroArch's
  (crt, handheld, ntsc, vhs, xbr...), and in the device chain's **Add > Retro Shaders**. A preset's
  settings (its *#pragma parameter*s) are knobs, starting at the preset's values.
- **Picture fed in**: the shaders draw a scanline, a pixel or a phosphor per line of the picture they
  get, as from an old console. It starts at **240 lines**; **144** and **160** suit the handheld ones
  (Game Boy, GBA), **As it is** gives the project's own lines.
- They run on the **GPU renderer** (the preview, renders, the Live tab), each pass as RetroArch runs it:
  sizes by source, viewport or pixels, float and sRGB passes, the frames before, lookup textures,
  aliases. The CPU renderer leaves the picture as it is.
- The picture is put on black first, so transparent parts come out black.

## Video scopes

The **Scopes** button over the preview (**Ctrl+Alt+S**) shows a scope beside the preview, live
while playing:

- **Waveform**: the brightness of each column, 0-100%.
- **RGB Parade**: the same for red, green and blue side by side.
- **Vectorscope**: hue and saturation, with the 75% colour-bar targets and the skin tone line.
- **Histogram**.

Use the *Color bars (SMPTE)* generator to check them: each bar lands on its vectorscope target.

## Try it

- [Colour Grading and Scopes](tutorials/05-colour-grading.md)
- [Glitch and Retro Looks](tutorials/06-glitch-and-retro.md)
- [Masks, Stabilising and Removing Backgrounds](tutorials/12-masks-stabilize-background.md)
