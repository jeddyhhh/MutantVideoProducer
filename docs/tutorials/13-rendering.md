# 13. Rendering and Sharing

You learn: Render As, choosing a template, rendering for a phone, keeping under a size limit,
rendering part of the timeline, GPU encoding, and saving single frames.

**Example project:** [first-edit](../../data/examples/first-edit.mvp), or any project of your own.

## 1. For YouTube

1. Press **Ctrl+R** (**File > Render As...**).
2. Type *youtube* in the search box and choose **YouTube 1080p**. The notes under it give YouTube's limits.
3. Click **Browse...** and choose where to save.
4. Click **Render**. A progress bar shows how far it is.

## 2. Faster, on the graphics card

1. Press **Ctrl+R** again and choose the same template.
2. Tick **Encode on the GPU when possible**. On a computer with NVIDIA, AMD or Intel graphics, this
   is many times faster. (If no encoder works, the box says so.)

## 3. For TikTok, Reels or Shorts

The example is 16:9; phones want 9:16.

1. Choose **TikTok (1080x1920)**.
2. Set **Other shapes** to **Fit on a blurred copy of the picture**: the picture is fitted in the
   middle over a blurred, enlarged copy of itself, with no black bars.
3. Try **Fill (crop the edges)** too: the middle of the picture fills the frame.

## 4. Under a size limit

1. Choose **Discord (free, 20 MB)**. The bitrate is chosen so the whole file stays under 20 MB.
2. For any other limit, pick a template and set **Fit file into** to the size you need.

## 5. Only part of it

1. Close the dialog. Drag in the bar above the ruler to select a few seconds.
2. Press **Ctrl+R**. Set the **Render range** to **Loop region only**.

## 6. Other formats

Browse the template groups: *Editing & Mastering* (ProRes, DNxHR) for handing a video to another
editor, *Animation* for a GIF, *Audio Only* for just the sound, *Image Sequences* for a folder of
PNG frames, *Retro & Legacy* for a PSP or an iPod.

Choose **Custom** to pick any format and codec yourself, and set the quality, frame size and rate,
pixel format (10-bit, 4:2:2, 4:4:4), audio codec and bitrate.

## 7. A single frame

1. Put the cursor on a nice frame.
2. **File > Export Frame as Image...** (**Ctrl+Shift+F**) saves it at full size as PNG, JPEG or WebP.
3. **Ctrl+Shift+C** copies it to the clipboard, to paste into another program.

## Next

- [Performing Live](14-performing-live.md)
- [Rendering](../rendering.md) in the guide lists every template group.
