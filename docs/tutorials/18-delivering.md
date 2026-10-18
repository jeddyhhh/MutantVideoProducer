# Tutorial 18: Delivering a Finished Edit

Check a finished edit, render it for several platforms at once and write its publishing details, all from the
**Deliver** tab. Allow 10 minutes. Use any project you have finished, or the
[first-edit](../../data/examples/first-edit.mvp) example.

## 1. Open the tab and read the checks

1. Click **Deliver** at the top of the window.
2. The checks run straight away: offline media, gaps with no picture, loudness, YouTube chapters, length limits and
   captions. Green is fine, amber is worth a look. **Go There** jumps to a problem in the Editor.
3. **Save** the project if it says *Unsaved project*. Outputs and details you chose before the first save carry over,
   and the files take the project's name.

## 2. Check the loudness

1. Choose a target (YouTube: −14 LUFS) and press **Measure**.
2. If it is amber, press **Fix...** to open Audio Finishing. With a **Limiter** in Master FX, **Apply safe gain** brings
   the mix right up to the target.

## 3. Choose the outputs

1. **Add a Pack**: *YouTube* (1080p and Shorts) and *Audio* (MP3 and WAV), for example.
2. Each output shows its size, codecs, estimated file size and file name. Untick any you don't need.
3. A Shorts output longer than 3 minutes gets a **Length limits** check: **Trim** renders just the first 3 minutes.
4. Under the list, the total size and the free space in **Folder** (red if it might not fit).

## 4. Render them all

1. Press **Render**. The ticked outputs go through the [Render Queue](../rendering.md) one after another, each with its
   own progress, and the bar on top shows the whole batch with the time left.
2. To stop part-way, stop the Render Queue: the tab then says *Stopped: 2 of 4 rendered*, and **Render** starts the
   rest again.
3. Finished files appear in **History**, newest first, with their lengths checked.

## 5. The publish pack

1. Type a title, a description and tags. In the description, `{chapters}` becomes the YouTube chapter list,
   `{tracklist}` the music, and `{title}`, `{length}` and `{date}` what they say.
2. For the thumbnail, click a frame in the strip, or move the cursor in the Editor and press **Use Cursor Frame**.
3. **Save Pack** writes the description, tags, captions and thumbnail next to the renders, ready to upload.

The details of every part are in [Deliver](../deliver.md). An AI assistant can do the same with the `deliver` tool
([AI Control](../ai-control.md)).
