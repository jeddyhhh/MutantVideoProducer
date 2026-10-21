# Audio

For final delivery levels, see [Audio finishing](audio-finishing.md): LUFS targets,
true peaks, mix checks and undoable master gain.

## Audio effects

Drag an effect from the **Audio FX** tab:

- onto an audio event (event FX),
- onto an audio track's header or empty space (track FX),
- or into the **Master FX** (the button on the transport bar, or **Edit > Audio Master FX...**) for the final mix.

Setting, keyframing and grouping them works as for video: see [Effects](effects.md).

| Effect | What it does |
|---|---|
| Gain | Volume change in dB, optional phase invert. |
| Parametric EQ | Low cut, low shelf, two bell bands, high shelf and high cut. Bands at 0 dB are off. |
| Filter | Low-pass, high-pass, band-pass or notch, resonant; 24 dB/octave stacks two stages. |
| Compressor | Evens out loud and quiet parts. Stereo-linked, soft knee. |
| Limiter | Brick-wall, with 2 ms lookahead: nothing goes over the ceiling. |
| Noise Gate | Silences sound below the threshold (hum, room noise between words). |
| Reverb | Room reverb (Freeverb). On a track, the tail rings out past the end of events. |
| Delay / Echo | Repeating echoes, optionally bouncing between left and right. |
| Chorus | Thickens the sound with slowly modulated copies. |
| Flanger | A jet-plane sweep. |
| Stereo Tools | Width, balance, channel swap, per-channel phase invert, mono. |
| Noise Reduction | Removes steady background noise (hiss, hum, fans) with FFmpeg's FFT denoiser. |
| Pitch Shift | Changes the pitch without changing the speed (Rubber Band). |
| VST3 Plugin | Runs an installed VST3 effect (see below). |
| FFmpeg Audio Filter (advanced) | Any FFmpeg audio filter chain, for example `loudnorm`, `dynaudnorm`, `deesser`. |

Effects with built-in delay (pitch shift, noise reduction, the limiter, loudness normalisers) are
delay-compensated on events, tracks and the master bus, so the sound stays in sync with the picture.

## Sound effects library

The **Sounds** tab (next to Profile Media) holds a library of short sound effects that comes with every installation:
whooshes, swooshes, risers and glitches for transitions; booms, a bass drop, punches and explosions; comedy stings
(air horn, sad trombone, rimshot, record scratch, boing, crickets, "dun dun dunnn", drum roll, ta-da, giggling,
applause); dings, pops, notifications, typing and a camera shutter; and everyday sounds such as footsteps, doors,
coins and a heartbeat.

- Type in the search box to find a sound by name.
- Double-click a sound (or press **Play**) to listen.
- Drag it onto an audio track, or press **Add at Cursor**: it's added to Project Media and placed on the timeline.

Every sound is public domain (CC0): use them in any video, commercial ones too, with no credit needed. Most were made
for Mutant Video Producer; the rest come from Kenney (kenney.nl), OpenGameArt.org and the U.S. Fish & Wildlife
Service. **Credits** opens the list of where each one came from. AI control: `list_sound_effects`.

## Audio presets

About 100 factory presets.

- **Under each effect**, for example: EQ *Vocal Presence*, *Bass Boost*, *Clear Muddiness*;
  Compressor *Podcast Leveler*, *Punchy Drums*; Reverb *Small Room*, *Cathedral*, *Plate*;
  Delay *Slapback*, *Ping-Pong Echo*; Pitch *Chipmunk*, *Deep Voice*, *Monster*;
  FFmpeg *Loudness Normalize (EBU R128)*, *De-esser*.
- **Preset Chains**:
  - *Voice*: Podcast Voice, Radio DJ, Clean Up Dialogue, Audiobook Narration, Chipmunk, Monster,
    Robot, Whisper.
  - *Character*: Telephone, Walkie-Talkie, Megaphone, AM Radio, Lo-Fi, Underwater, Next Room.
  - *Space*: Stadium PA, Cathedral, Concert Hall, Cave Echo, Space Echo, Haunted.
  - *Cleanup & Mastering*: Remove Hum (50 or 60 Hz), Noise Cleanup, Broadcast Loudness, Music
    Under Voice, Master Glue.
  - *Racks*: Parallel Compression, Wide Doubler, Reverb Send.

Saving your own presets is described under [Effects](effects.md#presets).

## VST3 plugins

1. Drag **VST3 Plugin** (Audio FX > Plugins) onto an event, a track or the master.
2. Click the **🔌** button on the device. It lists the plugins installed in
   `C:\Program Files\Common Files\VST3` and your own `Programs\Common\VST3` folder.
   Shell plugins that hold several (such as Waves) ask which one.
3. The plugin's own window opens. What you change there is saved with the project and can be undone.
4. **Dry / wet** mixes the plugin with the sound before it.

Every mixer and the render play the plugin the same way.

## The Audio Mixer

**View > Audio Mixer** (**Ctrl+Shift+M**, or **Mixer** on the transport bar): a strip per audio track with:

- a stereo peak meter, with peak hold and a clip light (click to reset),
- the volume fader (double-click the value for 0 dB),
- pan, mute and solo,
- its effects,
- **Duck under** (see below).

The master output has a meter too. The transport bar also shows a master peak meter during
playback; click it to reset the clip lights. Each audio track's header has a level meter while playing.

## Auto ducking

Auto ducking turns music down while someone speaks.

1. Set the music track to **Duck under** the voice track: in the Mixer, or right-click the music
   track's header > **Auto Duck Under**.
2. Choose how far: 6 to 36 dB (12 by default).

While the voice track has sound, the music comes down in about 25 ms. It holds 0.3 s after the
voice stops, then recovers over about 0.5 s. A *duck* badge shows on the track header. It works in
playback and in renders.

## Track listing for YouTube

Right-click an audio track's header > **Track Listing for YouTube...** lists its events in start
order, ready for a video description:

```
01. Beta Boyz - 00:00
02. Not A Chance - 04:19
```

Names come from the file names, without the extension or a leading track number; a renamed event
keeps its name. Past an hour, times show hours (`1:02:05`). Edit the names in the box, then **OK**
copies the list.

## Volume and pan

- Each track's **Vol** and **Pan** bars, and its volume and pan [envelopes](editing.md#envelopes).
- Each event's level line and fades (see [Editing](editing.md#fades-and-level)).
- Right-click an audio event > **Normalize** (or **Edit > Normalize Audio Events**) sets its peak to
  -0.1 dBFS by adding a Gain effect.

## Audio scrubbing

Dragging the cursor on the ruler, or stepping frames, plays a short snippet of the mix there, so
you can find a cut by ear. **Options > Audio Scrubbing** turns it off.

## Detect Beats

**Tools > Detect Beats** (**Ctrl+B**, or right-click an audio event) finds the beat of the music and
puts markers on it.

- **What it listens to**: the time selection, else the selected events, else the whole timeline.
  It uses the selected audio events alone if there are any, otherwise the whole mix.
- **Markers on**: every beat, every bar (4 beats), every 2 bars or every 4 bars. Bars are numbered,
  the first beat counting as the 1. The grid is carried back to the start, as the tracker needs a
  few seconds to lock on.
- **Listen to**: everything, or the kick drum.
- **Replace the markers already there**.
- It also sets the beat [grid](editing.md#snapping-and-the-grid)'s tempo.

Then:

- **Tools > Split Selected Events at Markers** cuts the selected events on every marker.
- **Tools > Make Beat Montage...** fills a new video track on top with clips cut every 1 to 16
  markers. The clips come from the files selected in Project Media (or all video files), in turn or
  shuffled, each from a random part of its file, over the time selection or the whole timeline.

## Try it

- [Mixing Audio](tutorials/07-mixing-audio.md): narration over music, ducked and cleaned up.
- [Cutting to the Beat](tutorials/09-cutting-to-the-beat.md).
