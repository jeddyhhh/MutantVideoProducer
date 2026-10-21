# Tutorial 17: An Animated Short with Lip Sync

Make a short cartoon entirely inside Mutant Video Producer: characters built from shapes, voices from local speech
generation, and mouths that move with each character's own lines. This is how the *Snowbank* example cartoons were
made. Allow 30 minutes.

You need **Speech Generation** installed (**Tools > AI Components...**) and at least one voice downloaded (see
[Captions and Subtitles](../captions.md#generate-voice-from-captions)).

The quickest way to the voices: **Tools > Captions > Generate Dialogue from a Script...** with the script as
`Name: line` lines gives every character a voice, a pitch and their own track in one go
([how](../captions.md#generate-dialogue-from-a-script)); then skip to step 3. Steps 1 and 2 do the same by hand.

## 1. Write the lines as captions

1. Start a new project (**File > New**), 1920×1080 at 24 fps, which suits cut-out animation.
2. Add a text track and type each line of dialogue as a caption, in order, a second or two apart. Put the
   speaker's name at the start of the event name (for example `Gus: Did you guys hear?`) so you can find them later.

## 2. One voice track per character

1. Select every caption of one character (Ctrl+click), then **Tools > Captions > Generate Voice from Captions...**
2. Set the source to **Selected captions only**, pick a voice and press **Generate Voice**. A new **Voice** track appears.
3. Double-click the track's name and rename it after the character: `Gus`.
4. Repeat for each character. With only a few voices installed, give characters their own sound with a
   **Pitch Shift** on their track (drop it onto the track header; +4 semitones makes a child, −4 a giant), or an
   **FFmpeg Audio Filter** such as `aecho=0.8:0.9:7:0.45` for a robot.

Keep every character on their own track: that is what the lip sync follows.

## 3. A blank scene

1. On the **Generators** tab, drag **Blank Graphics Canvas** (in the Graphics category) onto a video track and make it
   as long as the shot.
2. Select it and choose **Compose Graphics…** in Properties.

## 4. Build a character

For a head start, drag **Animation: characters** instead of the blank canvas: two ready-made characters that talk,
gesture and blink. Bind their voice tracks as `voice_a` and `voice_b` (step 5) and change their colours in the
scene variables (`a_jacket`, `b_hat`...). To build your own:

On the **Layout** tab:

1. Choose **ellipse** in the layer picker and press **Add layer**: one for the head, two white ones for eyes, two
   small black ones for pupils, and one for the mouth (dark red).
2. Select the head, eyes, pupils and mouth and press **Group selected layers**; name the group `Gus`. Moving or scaling the group moves the
   whole character. A group's children use fractions of the group's box, so the character can be copied and resized.
3. For a body, a hat and arms, add **path** layers (on the **Motion** tab, **Edit curve points on canvas** and
   **Append path point** shape them). Give the arms their own
   group with the anchor at the shoulder, so they turn from there. Hang the arms from *inside* the body and draw them
   behind it: the shoulder joint stays hidden however far the arm turns.

## 5. Lip sync

1. On the **Scene** tab, press **Bind audio for animation…**
2. In the list, choose **Track: Gus (its events during this scene)** and type the binding name `gus`. Every Gus line
   during the scene is analysed at its place on the timeline. Press OK.
3. Select the mouth, open **Layer data** and give it:

   ```json
   "anchor_y": 0,
   "scale_y": {"audio": "gus", "base": 0.1, "amount": 2.4, "channel": "peak", "attack": 0.02, "release": 0.1}
   ```

   The mouth is nearly shut when Gus is quiet and opens with his voice. `anchor_y` 0 keeps its top still.
   Without JSON: on the **Keys** tab choose the mouth and the property `scale_y`, and in **Follow audio** pick `gus`,
   0.1 when silent, 2.4 at full volume, and press **Follow**.
4. **Apply data to preview** and play: the mouth flaps through all of Gus's lines.

Bind each character in the scene the same way, each under their own name.

The same binding can drive more than the mouth. A few touches that make characters feel alive:

- **Talking bob**: the body group's `y` as `{"audio": "gus", "base": 0, "amount": -0.04}` lifts it as he speaks.
- **Gestures**: an arm group's `rotation` as `{"audio": "gus", "base": 10, "amount": 25}` swings it out with his words.
- **Blinking**: the eyes group's `scale_y` keyed 1 → 0.08 → 1 over a tenth of a second, with `loop` set to a few
  seconds (give each character a different loop so they don't blink together).
- **Looking at the speaker**: key the pupils' group `x` a little left or right when someone else starts talking.
- **Breathing**: the whole character's `scale_y` from 1 to 1.012 and back, `ping_pong`, every two seconds.

## 6. Camera and cuts

On the **Keys** tab, choose the camera:

- A **close-up**: key `zoom` to 2 and `x`/`y` to the speaker's face at the start of a line, and back to 1 at its end.
  Put two keys a frame apart (1/24 s) for a hard cut, or further apart for a push in.
- A **dutch angle**: key the camera's `rotation` to −8 for a tense line.
- A **hit**: shake the camera's `x` by ±0.01 for a fifth of a second. Leave the wide shot at zoom 1.025 so a shake
  never shows past the edge of the picture.

## 7. Sound and finishing

1. Import music and sound effects (CC0 packs such as Kenney's work well) onto their own tracks.
2. **Tools > Audio Finishing...**: add a **Limiter** to **Master FX** first, then **Apply safe gain**. The gain goes in
   front of the limiter, so even a quiet mix with loud peaks reaches −14 LUFS for YouTube. See
   [Audio Finishing](../audio-finishing.md).
3. Render with **Render As**, or send it to every platform from the [Deliver](../deliver.md) tab.

## With an AI assistant

With **Options > Allow AI Control (MCP)** on, an assistant can do all of this: `generate_voice` for the lines,
`edit_event` to move each onto its character's track, `graphics_overlay` `create` for the scenes and
`bind_audio` with `source_track` (the character's track index) and `binding` (their name) for the lip sync. See
[AI Control](../ai-control.md) and [Animation scenes](../animation-scenes.md).

Next: [Delivering a Finished Edit](18-delivering.md).
