# 7. Mixing Audio

You learn: the Audio Mixer, auto ducking, volume envelopes, voice chains, master effects, audio
presets and VST3 plugins.

**Example project:** [audio-mixing](../../data/examples/audio-mixing.mtg)

## What's in it

- **Narration**: a voice starting at 2 seconds, with the *Podcast Voice* preset chain on its track.
- **Music**: set to **duck under** the narration by 14 dB, with a volume envelope that fades it in
  over the first 2 seconds and out over the last 3.
- A **Limiter** on the master.
- An aurora picture and a title.

## 1. Listen

Press **Space**. The music fades in, drops when the voice starts, comes back up when it stops, and
fades out at the end.

## 2. The Mixer

1. Press **Ctrl+Shift+M** (or **Mixer** on the transport bar).
2. Play and watch each track's meter. The clip light turns red if a track goes over 0 dB: click it to reset.
3. Drag the Narration fader up a little. Double-click the value to go back to 0 dB.
4. On the Music strip, **Duck under** shows *Narration*. Change the amount to 24 dB: the music now
   almost disappears under the voice.

## 3. The volume envelope

1. Select the Music track and press **V**. The envelope line shows on the track.
2. Double-click the line at 20 seconds to add a point, and drag it down: the music dips there.
3. Right-click the point > **Smooth**.
4. Press **P** for a pan envelope, and make the music drift from left to right.

## 4. The voice chain

1. Click the Narration track's header: its **Track** chain shows the *Podcast Voice* devices.
2. Switch them off one at a time to hear what each does.
3. From the Audio FX tab, expand **Reverb** and drag **Small Room** onto the narration event: a
   little room sound on the voice only.
4. Try **Preset Chains > Character > Telephone** on the narration event.

## 5. The master

1. Click **Master FX** on the transport bar: the Limiter keeps the mix under its ceiling.
2. Choose its **Broadcast -1 dB** preset with the device's **≡** button.

## 6. Normalize

Right-click the narration event > **Normalize**: its peak goes to -0.1 dBFS (a Gain effect is added).

## 7. A VST3 plugin

If you have VST3 effects installed:

1. Drag **VST3 Plugin** (Audio FX > Plugins) onto the Music track.
2. Click **🔌** on the device and choose a plugin. Its own window opens.
3. Change something in the plugin, then press **Ctrl+Z**: the change is undone.

## Next

- [Captions and Subtitles](08-captions.md) uses this project's narration.
- [Audio](../audio.md) in the guide.
