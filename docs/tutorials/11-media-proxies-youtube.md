# 11. Media, Proxies and YouTube

You learn: importing, organising Project Media (folders, ratings, search, in / out marks), proxies,
Create Clip from YouTube, relinking missing files, and recovering after a crash.

**Example project:** none needed. Start a new project (**Ctrl+N**). To have something to import,
use your own videos, or the example media in `Documents\Mutant Video Producer Examples\media` (there after you
have opened any example).

## 1. Import

1. Press **Ctrl+I** and choose a few files. Or drag them from Explorer into Project Media.
2. The first video sets the project's frame size and rate. Check them with **File > Properties**
   (**Alt+Enter**).
3. Drag a file straight onto a track from Explorer: it is imported and placed in one go.

## 2. Organise Project Media

1. Select two files, right-click > **Move to Folder**, and type `Footage/Day 1`.
2. Select a file and press **4**: four stars.
3. In the bar above the files, type part of a name in the search box; pick a folder; pick
   *at least 3 stars*. Clear them again.

## 3. In and out marks

1. Right-click a video > **Mark In / Out...**.
2. Scrub to where the good part starts and press **I**, then where it ends and press **O**.
3. Double-click the file: only the marked part is placed at the cursor.

## 4. Proxies

For heavy footage (4K, HEVC, ProRes, 10-bit) the preview can play light copies:

1. Right-click a file > **Create Proxy**. The status bar shows the progress, and the file gets a blue **P**.
2. Toggle **Proxies** over the preview to compare.
3. Renders always use the originals.
4. **Tools > Make Proxies on Import** makes them for heavy files as you import them, and **Tools >
   Proxy Size** sets their size.

## 5. Create Clip from YouTube

1. Copy the link of a video you have the right to use (your own, or Creative Commons).
2. Press **Ctrl+Shift+Y** (**File > Create Clip from YouTube...**). The link is already filled in.
3. Type the part you want, for example *0:30* to *0:45*, and choose **Up to 1080p**.
4. Click **Create Clip**. The first time, Mutant Video Producer asks to download yt-dlp (about 18 MB).
5. The video is saved in a *YouTube* folder next to the project, appears in Project Media's
   *YouTube* folder with that part marked, and is placed at the cursor.

## 6. Relink a moved file

1. Save the project (**Ctrl+S**) and close Mutant Video Producer.
2. In Explorer, move one of the project's files into another folder.
3. Open the project: **Missing Media** opens, and the event shows *Media offline*.
4. Click **Locate** and choose the file in its new folder. If other files moved with it, they are
   found too.

## 7. Autosave and recovery

1. Make a change and don't save.
2. Wait 2 minutes: Mutant Video Producer keeps a copy in the background (**Options > Autosave** sets how often).
3. If Mutant Video Producer ever crashes or the computer turns off, the next start offers to recover the copy.

## 8. From Live

Clips recorded on the Live tab appear in the **From Live** tab, newest first. Double-click one to
place it at the cursor. See [Performing Live](14-performing-live.md).

## Next

- [Masks, Stabilising and Removing Backgrounds](12-masks-stabilize-background.md)
- [Media](../media.md) in the guide.
