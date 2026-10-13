# 8. Captions and Subtitles

You learn: Auto Captions, caption styles, editing captions, translating, subtitle files, and
subtitle tracks in renders.

**Example project:** [audio-mixing](../../data/examples/audio-mixing.mtg), for its narration.

## 1. Auto Captions

1. Press **Ctrl+Shift+T** (**Tools > Captions > Auto Captions**).
2. Choose the **Speech model**. *Base* is a good start. A model not on this computer yet is
   downloaded first (Base is 148 MB), after asking.
3. Leave **Language** on *Detect it*, **Style** on *Subtitles*, and **Caption length** on 42 characters.
4. Click **Add Captions**. A **Captions** track appears with a text event for each sentence, starting
   and ending with the voice.
5. Play it.

## 2. Edit the captions

1. Click a caption. Its settings open in the **Properties** tab: correct a word in **Text**.
2. Change the font, size, outline and box like any title (see
   [Titles and Generators](02-titles-and-generators.md)).
3. To restyle them all at once, select all the captions (click the first, then Ctrl+click the
   others) before changing the style.
4. Drag a caption's edge to make it stay longer.

## 3. Kinetic captions

1. Press **Ctrl+Z** to remove the captions.
2. Run Auto Captions again with **Style** set to *Kinetic Text*: the words pop on in time with the voice.

## 4. Translate

Run Auto Captions with **Translate into English** ticked. Whatever language is spoken, the captions
are in English. (The narration is already English; try it on a video in another language.)

## 5. Who is speaking

With a recording of two people, choose *Small English, marks who is speaking*: the second speaker's
captions are yellow.

## 6. Subtitle files

1. **Tools > Captions > Export Subtitles...** with a caption selected writes the track as **SRT** or **WebVTT**.
2. **Tools > Captions > Import Subtitles...** reads an SRT or WebVTT file onto a new Captions track.

## 7. Subtitles viewers can switch on and off

1. Press **Ctrl+R**.
2. Choose an MP4 template and tick **Add the captions as a subtitle track**.
3. Render, and open the file in a player such as VLC: the subtitles can be switched on and off.

Burned-in captions are part of the picture. With the subtitle track you can also switch the
Captions track off (**M** in its header) so the picture stays clean.

## Next

- [Cutting to the Beat](09-cutting-to-the-beat.md)
- [Captions and Subtitles](../captions.md) in the guide.
