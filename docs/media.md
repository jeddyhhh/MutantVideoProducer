# Media

## Importing

- **Ctrl+I** (**File > Import Media...**) opens files.
- Drag files from Explorer into **Project Media**, or drop them straight on a track.
- Mutant Video Producer reads anything FFmpeg can: more than 360 container formats and about 520 decoders, from
  AV1 and HEVC 10-bit to old phone `.3gp` clips, RealMedia, DV, still images and image formats.
  **Help > Supported Formats...** lists them.
- The first video imported into an empty project sets the project's frame size and frame rate,
  unless you chose a shape in [File > New](getting-started.md#starting-a-project). **File >
  Properties** (**Alt+Enter**) changes them later.

### Placing files

- **Drag** a file from Project Media onto a track, or **double-click** it to place it at the cursor.
- Dragging a video onto the timeline, from Project Media or Explorer, creates new tracks on top:
  **Clip1-Video** and, if the file has sound, **Clip1-Audio** directly underneath. Both tracks share
  the same clip number, which increases for each imported video. Each video in a batch gets its
  own adjacent pair. Double-clicking still places media on the selected or existing tracks.
- Dragging a still image creates its own orange **Image** track (**Image 1**, **Image 2**, ...).
  Image tracks support visual effects, Pan/Crop, opacity and blending, and are included in renders.
- On an empty timeline, the first file goes to the start.
- A file dropped within 3 seconds of the end of the last event on a track joins on to it. Hold
  **Shift** while dropping to place it exactly where you let go.
- A video file is placed as a **grouped pair**: its picture on a video track and its sound on an
  audio track. A link icon on their title bars shows the group. They move, trim and split together.
  See [Groups](editing.md#groups).
- Dropped into the middle of the timeline with auto ripple on, the file is inserted: what comes
  after moves along.

## Project Media

The **Project Media** tab lists the project's files, with thumbnails.

- **The filter bar** above the files:
  - The search box finds files by name or folder.
  - The folder list shows one folder.
  - The stars list shows only files rated at least that many stars.
- **Folders**: select files, right-click > **Move to Folder**. Type a name such as `Footage/Day 1`
  for a folder inside a folder.
- **Ratings**: right-click > **Rating**, or select files and press **0** to **5**.
- **Mark In / Out**: right-click a file > **Mark In / Out...** to scrub through it. Press **I** and
  **O** to set the part you want. Placing the file then uses only that part.
- **Relink Media**: right-click > **Relink Media** (see [Missing media](#missing-media)).
- **Create Proxy / Delete Proxy**: see [Proxies](#proxies).
- Folders, ratings and marks are saved with the project.

## Create Clip from YouTube

**File > Create Clip from YouTube...** (**Ctrl+Shift+Y**) downloads a video and places it at the cursor.

1. Paste the link. A link on the clipboard is filled in for you. YouTube and the
   [other sites yt-dlp knows](https://github.com/yt-dlp/yt-dlp/blob/master/supportedsites.md) work.
2. Optionally give the part you want, for example *1:30* to *2:05*. Times can be `90`, `1:30` or
   `1:02:03`. Leave them empty for the whole video.
3. Pick the quality: up to 1080p, 720p, 480p or 360p, or **Sound only**.
4. For videos that ask you to sign in or verify your session, optionally select
   **Browser session** (Firefox, Chrome, Edge or Brave), or a Netscape-format
   **Cookies file**. Public downloads use no browser session by default.
5. Click **Create Clip**.

Windows builds include Deno for YouTube's JavaScript challenges. Other builds can
use Deno 2.3+ or Node 22+ from PATH. The official yt-dlp executable includes its
challenge solver scripts. Download progress and cancellation remain available.

Downloaded clips appear in the **From YouTube** tab next to **From Live**, as well
as the project's main media pool. The tab opens after an import. Drag clips onto
the timeline or double-click to add them at the cursor. Search, ratings, in/out
marks, relinking and proxies work there too. Download origin is saved with the
project and survives folder changes. Older projects' YouTube-folder media is
recognised when reopened.

The tab shows only this project's downloads. Tick **Show downloads from other projects** to
also list the profile's earlier downloads; double-click or drag one to import it.

### YouTube troubleshooting

An update fixes extractor changes; it cannot fix every YouTube access check.
Mutant Video Producer now distinguishes these failures rather than always suggesting an update:

- **Sign in to confirm you're not a bot:** open the video in your browser and
  complete YouTube's check. Select that browser session, or use an exported
  YouTube cookies file, then retry. Your session details are not stored in the project.
- **Cookies cannot be read/decrypted:** close the browser and retry. Chrome/Edge
  encryption may prevent extraction; Firefox or an exported cookies file can help.
- **Too many requests / try again later:** wait before retrying. Repeated updates
  and retries will not resolve a session or network rate limit.
- **JavaScript challenge failure:** check the runtime status in the dialog and
  update yt-dlp if needed.
- **403 / proof-of-origin token:** YouTube may require a video-specific token.
  A PO-token provider is a further setup option; Mutant Video Producer does not bundle one yet.
- **Unavailable/restricted video:** confirm the video is accessible in your browser
  with the selected session.

See the official [JavaScript setup guide](https://github.com/yt-dlp/yt-dlp/wiki/EJS),
[YouTube cookie guide](https://github.com/yt-dlp/yt-dlp/wiki/Extractors#exporting-youtube-cookies),
and [PO-token guide](https://github.com/yt-dlp/yt-dlp/wiki/PO-Token-Guide).
There is no downloader that guarantees access to every YouTube video. For your
own uploads, YouTube Studio's download/export options are another source of files
you can import into Mutant Video Producer.

### Downloaded files

- The whole video is downloaded, then placed. The part you asked for is set as its in / out marks,
  so only that part is placed. You can change the marks later to use more of it.
- The file is saved in a **YouTube** folder next to the project. An unsaved project uses
  `Videos\Mutant Video Producer YouTube`.
- The file goes in Project Media's **YouTube** folder.
- The first time, Mutant Video Producer downloads [yt-dlp](https://github.com/yt-dlp/yt-dlp) (about 18 MB),
  after asking you.
- Sites change often. Extractor/JavaScript failures offer a downloader update;
  authentication and rate-limit failures show the relevant recovery steps.
- You get the best picture the site has up to the quality you chose. Some videos only exist at a
  lower resolution.

Only use videos you have the right to use: your own, Creative Commons, or with permission.

## Proxies

Proxies are light copies of heavy footage (4K, HEVC, AV1, ProRes, 10-bit) that the preview plays
instead, so playback stays smooth. 4K HEVC 10-bit previews about 8 times faster.

- **Making them**:
  - **Tools > Make Proxies on Import (heavy files)** makes them in the background as heavy files
    are imported.
  - **Tools > Create Proxies for Heavy Files** makes them for every heavy file in the project.
  - **Tools > Create Proxies for Selected Media**, or right-click files in Project Media >
    **Create Proxy**, makes them for the files you chose.
- The status bar shows the progress; **✕** stops it. A file with a proxy gets a blue **P** in
  Project Media.
- The **Proxies** button over the preview switches between proxies and originals.
- **Renders, frame exports and stabilisation always use the originals.**
- **Tools > Proxy Size**: 360p, 540p or 720p. Proxies are H.264, with the source's exact frame times
  and colours.
- Proxies are kept in the app's cache and shared by every project that uses the file.
  Right-click > **Delete Proxy** removes one. **Tools > Delete All Proxies...** empties the cache.

## Missing media

A file that was moved, renamed, or is on a drive that isn't connected shows **Media offline** on
its events. Opening the project brings up **Missing Media**. You can also open it from:

- **File > Relink Missing Media...**
- right-click the event, or the file in Project Media > **Relink Media**

In the Missing Media window:

- **Locate** one file. The others are looked for next to it, and where their old folder seems to
  have moved.
- **Search Folder** finds all of them by name in a folder and its subfolders.

Events come back as they were, with their effects, trims and speed. If the new file numbers its
streams differently, events use its first video or audio stream.

## Autosave and recovery

- While a project has unsaved changes, Mutant Video Producer keeps a copy every 2 minutes in the app's data
  folder. **Options > Autosave** sets it to off, 1, 2, 5 or 10 minutes.
- Saving, or closing normally, removes the copy.
- If Mutant Video Producer crashes or the computer turns off, the next start offers to recover the copy. It opens
  under the project's own name, unsaved, ready for **File > Save**.

### Crash reports

When Mutant Video Producer crashes it writes a report: a `crash-<date>-<time>.txt` with what failed, where, and the last
lines it logged, and on Windows a `.dmp` minidump for a debugger. They go in the app's data folder under `crashes`
(on Windows `%LOCALAPPDATA%\Mutant Video Producer\Mutant Video Producer\crashes`). The next start offers to show a new
report; attach it when you report a problem. A library loaded later (a graphics driver, a runtime) can't take the
crash handler's place: it is put back every few seconds, and the log names the library that replaced it.

## Collect Project and Media

**File > Collect Project and Media...** creates a portable folder containing a `.mvp` copy of the current edit and its assets. Unsaved changes are included; the open project's filename, undo history and saved state stay unchanged.

Choose a new destination folder, or use **Choose Parent Folder...** to suggest a new folder there. Existing folders are never overwritten. You can include unused **Project Media** items and saved RVC models. The collection copies whole source files, preserving trims and editable clips, rather than rendering or shortening them.

Assets include nested-sequence media, generated narration and alternate takes, graphics images (including animated and variable-bound paths), LUTs, waveform sources and Live overlay images. Native graphics and animation data remain editable inside the project. Proxies and regenerable caches are rebuilt on the destination computer.

A cancellable progress window shows copying progress. A completed collection includes `collection-report.json`, listing copied files with SHA-256 hashes, missing sources and dependencies. Missing sources remain offline in the copy; the completion window calls them out. Installed fonts and VST plugins are not copied: install the report's listed dependencies for matching appearance and processing. Mutant Video Producer supplies its own voice runtime; saved RVC model files can be included for regenerating narration.

Move or share the **entire collection folder**, then open its `.mvp` file. Paths resolve against that folder and remain relative when the collected project is saved again. If you subsequently import assets from outside the folder, collect again before moving it. Cancelling or encountering a copy error removes temporary files and does not publish a partial collection.

The report records original source paths for troubleshooting; inspect it before sharing if those paths contain personal information.

## One window

Mutant Video Producer runs once: opening a `.mvp` project (a double-click in Explorer, with the installer's file
link) or a media file with Mutant Video Producer while it is already open sends it to the open window, which asks
to save the current project first. Starting Mutant Video Producer itself again opens a second window.

## Recent projects

- **File > Open Recent** lists the last 10 projects.
- The last project reopens when Mutant Video Producer starts (**Options > Reopen Last Project on Start**). If
  Mutant Video Producer closed while opening it, it isn't reopened again automatically.

## From Live

The **From Live** tab lists every clip recorded and every screenshot taken on the [Live](live.md)
tab, newest first, with thumbnails and lengths (screenshots say *Still*; on the timeline they are
stills, five seconds long to start with).

- Drag a clip onto the timeline, or double-click it to add it at the cursor.
- Right-click to add it to Project Media, show it in its folder, rename it, or move it to the
  recycle bin.
- A clip already in Project Media is reused, not imported twice.
