# Rendering

## Render As

**File > Render As...** (**Ctrl+R**) writes the timeline to a file.

1. Pick a **template**. The search box finds them by name, for example *tiktok*, *discord*,
   *prores* or *psp*.
2. Choose the **output file**.
3. Choose the **render range**: the entire project, or the loop region only (the time selection).
4. Click **Render**.

The template fills in the format and codecs. You can change any of them:

- **Format** and **Format options** (FFmpeg muxer options, such as `hls_time=6`).
- **Video**:
  - **Codec**, **Frame size** (or *Project size*), **Frame rate**.
  - **Quality** (CRF / q) or **Bitrate**, and the **Encoder preset**.
  - **Extra options**: FFmpeg encoder options, such as `profile=high:tune=film`.
  - **Pixel format**: Automatic, or 10-bit, 4:2:2 or 4:4:4 output.
- **Audio**: **Codec**, **Sample rate**, **Channels** (stereo or mono), **Bitrate**.
- **Encode on the GPU when possible**: switches H.264, HEVC and AV1 templates to the graphics
  card's encoder (NVIDIA NVENC, AMD AMF or Intel Quick Sync). Much faster.
- **Other shapes**: how a project of another shape fills the frame, for example 16:9 into a vertical
  9:16 template:
  - **Fit** (black bars),
  - **Fit on a blurred copy of the picture**,
  - **Fill** (crops the edges),
  - **Stretch**.
- **Fit file into**: chooses the video bitrate so the whole file fits a size, for messaging apps and email.
- **Add the captions as a subtitle track**: see [Captions](captions.md#subtitle-tracks-in-renders).

Each template shows notes such as the platform's length and size limits. The dialog warns when the
render is longer than the platform accepts.

## Render Status

Every export opens a Render Status window, including exports started through MCP. The window shows the current stage, elapsed time, a production ETA, average frames per second, timeline speed and output size.

- **Live statistics** reports produced frames and mixed audio samples, the actual renderer and codecs, pixel and sample formats, packet counts, encoded payload size and measured time spent compositing, converting, mixing, encoding and muxing.
- **Output and timeline** records the output settings, render range, project counts and caption settings.
- **Active clips and effects** identifies the clips, source files and clip/track effects being processed at the reported timeline position.
- **Stage log** records setup, rendering, encoder flushing, container finalization and file closure, with timestamps and errors.

Frame production reaching 100% does not mean the file is complete. The window shows finalization separately; MP4 fast-start can take additional time and has no reliable ETA. **Cancel render** requests cancellation; the window stays available while the worker releases the file. After completion or failure, use **Copy report**, **Open output folder** (successful exports) or **Close**.

MCP `render_status` retains its existing progress and completion fields and adds a `details` object with render counters, formats, renderer, stages, timing and active clips.

## The templates

There are about 170, in groups:

- **General**: H.264, HEVC (8 and 10-bit), AV1, VP9, VP8, H.266 / VVC, Theora.
- **YouTube**: 720p to 8K at 30 or 60 fps with YouTube's recommended bitrates, Shorts, and
  **Audio-Only YouTube** (album art, 5 fps): music over a still picture, with a tiny picture stream.
- **Social Media**: TikTok, Instagram (Reels, square, 4:5, Stories), Facebook, X, Threads,
  Bluesky, LinkedIn, Snapchat Spotlight, Pinterest, Reddit, Tumblr.
- **Video Sites & Streaming**: Vimeo, Twitch, Dailymotion, Rumble, Steam trailers, Kickstarter,
  Spotify video podcasts, HLS and MPEG-DASH streams.
- **Messaging & Sharing**: Discord (20, 50 or 500 MB), WhatsApp (16 MB), Telegram, Signal, email.
  The render is kept under the app's size limit.
- **Devices**: iPhone, iPad and Apple TV (HEVC), Android, PS5, PS4, Xbox, Steam Deck, Raspberry Pi,
  smart TVs, Chromecast and Roku, digital photo frames.
- **Retro & Legacy**: PSP, PS Vita, PS3, Xbox 360, iPod, Nintendo 3DS, Wii Photo Channel, 3GP
  phones, DivX / Xvid, WMV, Flash Video, Cinepak, Quake III RoQ.
- **Disc & Broadcast**: DVD, VCD, Super VCD (NTSC and PAL), Blu-ray, AVCHD, MiniDV, XDCAM-style
  MXF, broadcast MPEG-TS.
- **Editing & Mastering**: every ProRes profile, DNxHR LB to 444 (MOV and MXF), CineForm, HAP and
  HAP Q for VJ software, all-intra H.264, uncompressed v210, QuickTime Animation.
- **Archival & Lossless**: FFV1, lossless H.264 RGB, UT Video, HuffYUV.
- **Animation**: GIF, APNG, animated WebP.
- **Image Sequences**: PNG 8 and 16-bit, JPEG, TIFF, DPX, OpenEXR, TGA, BMP, WebP, JPEG 2000,
  JPEG XL, QOI.
- **Audio Only**: WAV 16 / 24 / 32-bit float, AIFF, FLAC, ALAC, MP3, AAC, ringtones, Opus, Vorbis,
  AC-3, WMA, WavPack, AMR.
- **GPU Encoders**: NVENC, AMF and Quick Sync templates, when this computer has the hardware.

Choose **Custom** to start from any FFmpeg format and encoder: about 150 output formats.

## Single frames

- **File > Export Frame as Image...** (**Ctrl+Shift+F**) saves the frame at the cursor, at the full
  project size, as PNG, JPEG or WebP.
- **File > Copy Frame to Clipboard** (**Ctrl+Shift+C**).

## What a render uses

- Renders always use the original files, never [proxies](media.md#proxies).
- With **Options > GPU Acceleration** on, the picture is rendered on the graphics card; the result
  is the same as on the CPU.
- Everything on the timeline is included: effects, transitions, speed, stabilisation, ducking, the
  master effects and VST3 plugins.

## Try it

The [Rendering and Sharing](tutorials/13-rendering.md) tutorial renders an example project for
YouTube, a phone and a messaging app.
