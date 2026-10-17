# Deliver

The **Deliver** tab, next to Editor and Live, gets a finished edit out: it checks the edit, renders
every output you need at once, and makes what goes beside the video when you publish it.

## The header and the strip

The header shows the title, the length, the frame size and rate, and how ready the edit is: **Ready
to deliver**, or how many checks are worth a look. **Render** renders every ticked output; while it
works, the header shows which one, a progress bar and about how long is left, then **Open Folder**.

The strip under it is the whole edit at a glance: frames along it, the chapters (or the stretches
between markers) under them, markers as small ticks, and stretches with nothing on screen in red.
Hover for the time. **Click** a frame to make it the thumbnail; **double-click** to go there in the
Editor.

## Checks

The checks run each time you open the tab, and again a moment after each edit while it's open.
**Check Again** runs them now. A green tick is fine, an amber mark is worth a look, and a grey *i* is
a note.

- **Unsaved project.** File names and History follow the project's name; **Save** saves it.
- **Offline media.** Events whose file is missing. **Go There** moves the cursor to the first.
- **Nothing on screen.** A second or more with no picture on any track. **Go There** shows it.
- **Loudness.** Choose a target and press **Measure** to scan the final mix (as in
  [Audio Finishing](audio-finishing.md)). The meter shows the integrated loudness against the
  target: green when it is within 1 LU, amber when not, red when the true peak is over the ceiling.
  **Fix...** opens Audio Finishing.
- **YouTube chapters.** From the story chapters or the markers. YouTube shows chapters only when
  there are three or more and each is at least 10 seconds; the check names the ones that break that.
- **Length limits.** A ticked output whose platform takes less than the whole edit (YouTube Shorts:
  3 minutes). **Trim** renders that output from the start up to the limit; **Whole Edit** undoes it.
- **Captions.** How many caption events go into the pack and into subtitle tracks.

## Outputs

Each output shows its frame size, rate, codecs and an estimated file size (`~` when it depends on
the picture, for quality-based templates), and the file it writes in **Folder**: the project's name
and the template's short name, such as `Trip - YouTube 1080p.mp4` or `Trip - MP3.mp3`.

- Pick a template in the list and press **Add**, or **Add a Pack**: YouTube (1080p and Shorts),
  Social (Reels, TikTok, square), Audio (MP3 and WAV), Masters (ProRes HQ and lossless FFV1) or
  Share (a small preview and Discord). **✕** takes an output out.
- The ticked outputs are rendered one after another through the [Render Queue](rendering.md), with
  the GPU encoder when there is one. Each shows its progress.
- Under the list: how many outputs, about how big they'll be together, and the free space where they
  go (in red when they might not fit).

The outputs, the ticks, the folder and the loudness target are remembered for each project file.
The folder starts as the project's own folder.

## Publish pack

- **Title.** Starts as the project's name. It is drawn on the thumbnail.
- **Thumbnail.** Click a frame in the strip, or **Use Cursor Frame**: 1280x720 with the title in
  large outlined type.
- **Tags.** Comma-separated, with YouTube's 500-character count.
- **Description.** With YouTube's 5000-character count. **Insert** puts in placeholders filled in
  when it is copied or saved: `{chapters}` (the YouTube chapter lines), `{tracklist}` (the chosen
  audio track's events as `01. Name - 04:19`, for an album video), `{title}`, `{length}` and `{date}`.
- **Copy Description** puts it on the clipboard, filled in. **Save Pack** writes the title,
  description and tags (`Name - description.txt`), the thumbnail (`Name - thumbnail.jpg`, under
  2 MB) and the captions (`Name.srt`) into the output folder.

## AI control

An AI assistant connected through [AI control](ai-control.md) can do all of this with the `deliver`
tool: read the checks, set up the outputs, render them, and write the title, description and tags
from the story chapters and the script, then save the pack. What it writes appears here to review.

## History

Every render of the project is listed, newest first: from this tab, **Render As**, the Render Queue
and AI control, with each file's length as read from the file (amber when it can't be read or isn't
as long as rendered) and size. **Play** (or a double-click) opens it, **Show in Folder** shows it, and
**Render Again** renders it again with the same template, over the same file.
