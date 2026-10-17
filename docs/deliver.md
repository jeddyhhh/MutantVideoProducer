# Deliver

The **Deliver** tab, next to Editor and Live, gets a finished edit out: it checks the edit, renders
every output you need at once, and makes what goes beside the video when you publish it.

## Checks

The checks run each time you open the tab, and again a moment after each edit while it's open.
**Check Again** runs them now.

- **Unsaved project.** File names and History follow the project's name; **Save** saves it.
- **Offline media.** Events whose file is missing. **Go There** moves the cursor to the first.
- **Nothing on screen.** A second or more with no picture on any track. **Go There** shows it.
- **Loudness.** Press **Measure Loudness** to scan the final mix (as in
  [Audio Finishing](audio-finishing.md)). The result is checked against the *Online video / speech*
  target (-16 LUFS, -1.5 dBTP); **Audio Finishing...** opens the full window to fix it.
- **YouTube chapters.** From the story chapters or the markers. YouTube shows chapters only when
  there are three or more and each is at least 10 seconds; the check names the ones that break that.
- **Length limits.** A ticked output whose platform takes less than the whole edit (YouTube Shorts:
  3 minutes). **Trim** renders that output from the start up to the limit.
- **Captions.** How many caption events go into the pack and into subtitle tracks.

A tick is fine, a warning sign is worth a look, and a dot is a note.

## Outputs

The ticked templates are rendered one after another through the
[Render Queue](rendering.md), with the GPU encoder when there is one. Each gets its own file in
**Folder**: the project's name and the template's short name, such as `Trip - YouTube 1080p.mp4` or
`Trip - MP3.mp3`. Pick a template in the list below and press **Add**; **Remove** takes the selected
one out. **Render** queues every ticked output and starts the queue; the State column follows each.

The outputs, the ticks and the folder are remembered for each project file. The folder starts as
the project's own folder.

## Publish pack

- **Title.** Starts as the project's name. It is drawn on the thumbnail.
- **Thumbnail.** **Use Cursor Frame** takes the picture under the timeline cursor, 1280x720 with the
  title in large outlined type.
- **Description.** Write it here. **Chapters** adds the YouTube chapter lines; **Track Listing**
  adds the chosen audio track's events as `01. Name - 04:19` lines, for an album video.
- **Copy Description** puts it on the clipboard. **Save Pack** writes the title and description
  (`Name - description.txt`), the thumbnail (`Name - thumbnail.jpg`, under 2 MB) and the captions
  (`Name.srt`) into the output folder.

## History

Every render of the project is listed, newest first: from this tab, **Render As**, the Render Queue
and AI control. **Open File** plays the selected one; **Open Folder** shows it in Explorer.
