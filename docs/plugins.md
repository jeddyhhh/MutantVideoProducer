# Plugins

A plugin adds commands to **Tools > Plugins**. Choose one, fill in its small form, press **OK**, and it does a job
on the open project: lays out lyrics, cuts a music video, cleans up a podcast, exports a shot list. Everything a plugin
does can be undone with one **Ctrl+Z**.

A plugin is just a folder with two small text files: `plugin.json` (its name and its form) and a script in
JavaScript (what it does). The script works through the same commands [AI Control](ai-control.md) gives an assistant,
so a plugin can do almost anything you can do by hand, and nothing more.

- [Using plugins](#using-plugins)
- [The plugins that come with it](#the-plugins-that-come-with-it)
- [Writing a plugin: a first one, step by step](#writing-a-plugin-a-first-one-step-by-step)
- [How the bundled plugins do things](#how-the-bundled-plugins-do-things)
- [Reference](#reference)
- [For AI assistants writing plugins](#for-ai-assistants-writing-plugins)

## Using plugins

- **Tools > Plugins** lists every plugin's commands. Choose one, fill in its form and press **OK**. The form remembers
  what you entered last time.
- **One Ctrl+Z** takes back everything the command did.
- **Tools > Plugins > Open Plugins Folder** opens the folder for your own plugins. To install one, put its folder in
  there (unzip it so that its `plugin.json` is directly inside its own folder). The menu is read again every time it
  opens, so there's nothing to restart.
- **A plugin you added asks before it runs** the first time, and again whenever its files change. Run only plugins
  from people you trust: a plugin can change the project, save and render files, and do anything else AI Control can.
- If a plugin has a mistake in it, the menu shows **Plugins That Didn't Load**, which says what's wrong.

A plugin can't read or write files on its own, and it can't go online. It can read only the files and folders you pick
in its form, and write only the file you name in a "save as" box in its form.

## The plugins that come with it

| Plugin | What it does | What you give it |
|---|---|---|
| **Lyric Video from LRC** | Each line of timed lyrics becomes a title on a new Lyrics track, until the next line; can move each line onto the nearest beat. | An `.lrc` file, a style, a font |
| **Chapters from a List** | `00:00 Intro` lines (like a YouTube description) become named markers; Deliver turns them into YouTube chapters. | A `.txt` file |
| **Music Video in One Click** | Finds the song's beats, cuts a montage of your footage on them (a new track on top) and rolls Director's Dice over it. | A song on the timeline, videos in Project Media, a look |
| **Fill the Gaps** | Fills every empty stretch of a video track with random pieces of the videos in Project Media. | Which track |
| **Batch Effects** | Puts an effect or a preset on many events at once (the selected ones, or a whole track), or takes every effect off them. | An effect or preset name |
| **Countdown or Timer** | A countdown (10 ... 1, then "GO!") or a running clock, one title a second, on a new track. | When, how long, a style and colour |
| **Podcast Clean-up** | Evens out the voices, ducks the music under them, can take out long pauses, and sets the loudness for podcasts, YouTube or broadcast. | Which track is the music |
| **Vertical Cut-down** (Social Cut-downs) | Renders part of the edit as a 9:16 video for Shorts, Reels or TikTok (Auto Reframe follows the subject) through the Render Queue, then leaves the edit as it was. | Where and how long, the file to save |
| **Brand Kit** | Your intro at the start (the edit moves along), your outro at the end and your logo in a corner in between. | A logo, an intro, an outro (any of them) |
| **Scoreboard from CSV** | A score that changes through a match: one title per row of `time,home,away,note`. | A `.csv` file, the team names |
| **Edit List from CSV** | Lays out a cut from rows of `file,in,out,name`, one shot after another, at the end of the edit. | A `.csv` file |
| **Restyle Captions** | One font, size, colour, box and position for every caption on the captions tracks. | The look |
| **Setlist from CSV** | The Live tab's setlist for a gig from rows of `song,bpm,scene,notes,pad set`. | A `.csv` file |
| **Genre Pads** | Fills eight Live pads with named effect chains for a style: ambient, techno, punk, retro games or psychedelic. | A style |
| **Photo Slideshow** | Every picture in a folder, slowly zooming and crossfading, at the end of the edit, with a song if you like. | A folder, seconds per picture |
| **Shot List Export** | Writes a CSV of every event (track, times, file, effects) for a cue sheet or a spreadsheet. The edit isn't changed. | The file to save |
| **Stream Highlights** | Cuts the moments of a stream recording where the chat was busiest into a highlight reel. | The recording and its chat log |

To try **Lyric Video from LRC**, put a song on the timeline and pick `example.lrc` from its folder (`plugins/Lyric
Video` next to the program).

The CSV plugins take a header row or none, and times as seconds (`75.5`) or with colons (`1:15`, `1:01:15`). For
example, **Scoreboard from CSV**:

```
time,home,away,note
0,0,0
12:30,1,0,Goal!
58:02,1,1,"Penalty, saved... then scored"
```

**Edit List from CSV** looks for files without a folder next to the CSV file: `interview.mp4,12.5,20,Hello`.
**Stream Highlights** reads a text log with a time at the start of each line (`[1:02:03] name: message`), a CSV with
the seconds first, or a Twitch chat JSON file.

## Writing a plugin: a first one, step by step

Here is the whole of **Chapters from a List**, built up from nothing. It reads a text file the user picks and adds a
named marker for each line.

### 1. Make its folder

**Tools > Plugins > Open Plugins Folder**, and make a folder in there called `Chapters from a List`.

### 2. Describe it: plugin.json

`plugin.json` gives the plugin a name and describes each command: its menu entry, its script and the boxes in its
form. Each box is a **field** with an `id`, which is how the script will find what was entered.

```json
{
  "id": "chapters-from-list",
  "name": "Chapters from a List",
  "version": "1.0",
  "author": "Your Name",
  "description": "Turns lines like \"00:00 Intro\" into named markers.",
  "commands": [
    {
      "label": "Chapters from a List...",
      "script": "main.js",
      "fields": [
        { "id": "list", "label": "Chapter list (.txt)", "type": "file", "filter": "Text (*.txt);;All files (*)" },
        { "id": "replace", "label": "Remove the markers already there first", "type": "bool", "default": false }
      ]
    }
  ]
}
```

That makes a form with a file box (and **Browse...**) and a tick box.

### 3. Write what it does: main.js

The script sees one object, `mvp`. `mvp.args` holds what was entered in the form, by field id; `mvp.readText` reads
the file that was picked; `mvp.call` runs one of the program's commands.

```javascript
const timePattern = /\[?(?:(\d+):)?(\d{1,2}):(\d{2}(?:\.\d+)?)\]?/;   // 2:15, 02:15, 1:02:15, [2:15]

const chapters = [];
for (const line of mvp.readText(mvp.args.list).split(/\r?\n/)) {
    const m = timePattern.exec(line);
    if (!m)
        continue;                                   // (a line without a time)
    const seconds = Number(m[1] || 0) * 3600 + Number(m[2]) * 60 + Number(m[3]);
    const name = line.replace(m[0], "").replace(/^[\s\-–—:|]+|[\s\-–—:|]+$/g, "").trim();
    chapters.push({ time: seconds, name: name || "Chapter " + (chapters.length + 1) });
}
if (!chapters.length)
    throw new Error("no lines with a time in " + mvp.args.list);

if (mvp.args.replace)
    mvp.call("remove_markers", {});
for (const c of chapters)
    mvp.call("add_marker", { time: c.time, name: c.name });

mvp.log(chapters.length + " chapter markers");
```

- `mvp.call("add_marker", { time: 15, name: "The middle" })` is the program's **add_marker** command. Every command,
  with its arguments, is on the [Plugin Commands](plugin-commands.md) page.
- `throw new Error("...")` stops the command and shows the message.
- `mvp.log("...")`: the last message shows in the status bar when the command is done.

### 4. Try it

Open **Tools > Plugins** (the menu reads the folder again), choose **Chapters from a List...**, say yes to running
it, pick a text file, press **OK**. If the script has a mistake, a message says which file and line, and what went
wrong (`main.js:12: TypeError: ...`). Fix it, save the file and choose the command again: there's nothing to reload.
**Ctrl+Z** takes back each try.

### 5. Share it

Zip its folder. Whoever gets it unzips it into their plugins folder.

## How the bundled plugins do things

The bundled plugins (`plugins` next to the program, or
[data/plugins](https://github.com/jeddyhhh/MutantVideoProducer/tree/main/data/plugins) in the source) are short and
commented. Here are the ways of working they use; copy from them freely.

### Looking at the project

`mvp.call("get_project")` answers with everything: `duration`, `width`, `height`, `fps`, `cursor`, `media` (each with
`media_id`, `path`, `seconds`, `width`, `height`, `still`, `audio`), `tracks` (each with `index`, `type`, `name`,
`locked` and `events`), `markers` and `selected`. Each event has `event_id`, `start`, `end`, `length`, and either
`media_id` and `source_in` (a file) or `generator` and `text` (a title), plus `effects` (each with `effect_id` and
`type`) and `selected: true` when it's selected on the timeline.

**Batch Effects** works on the selected events:

```javascript
for (const t of mvp.call("get_project").tracks)
    for (const e of t.events)
        if (e.selected && !t.locked)
            mvp.call("add_effect", { target: "event", id: e.event_id, effect: "vhs" });
```

Ids stay the same while the plugin runs, but **ask again after a change**: `get_project` is a snapshot of the moment
it was called.

### Reading files the user picks

A `file` field gives the script a path it may read with `mvp.readText(path)`. **Lyric Video**, **Chapters from a
List**, **Scoreboard from CSV**, **Edit List from CSV** and **Setlist from CSV** all read text this way. The CSV ones
share a small `cells(line)` function that splits a CSV line and keeps `"quoted, cells"` whole; copy it from any of
them. A `files` field gives a list of paths (all readable), and a `folder` field gives a folder whose files may be
listed with `mvp.listFolder(folder, ["*.jpg", "*.png"])` and read. **Photo Slideshow** does:

```javascript
const pictures = mvp.listFolder(mvp.args.folder, ["*.jpg", "*.jpeg", "*.png"]);
const ids = mvp.call("import_media", { paths: pictures }).media_ids;
```

### Writing a file

A `save` field asks for a file name (with the usual "save as" window); that one file, and no other, may be written
with `mvp.writeText(path, text)`. **Shot List Export** writes its CSV that way.

### Adding a track and titles on it

```javascript
const track = mvp.call("add_track", { type: "video" }).index;   // a new video track goes on top: index 0
mvp.call("edit_track", { track: track, name: "Countdown" });
mvp.call("add_text", { preset: "title", text: "3", at: 10, length: 1, track: track, params: { color: "#ffcc00" } });
```

Text presets include `title`, `kinetic_text`, `lower_third`, `subtitle` and `title_subtitle`; `params` takes the
text's settings, such as `font`, `size` (percent of the picture's height), `color`, `x`, `y` (0 to 1) and `box`
(0 to 1, the box behind it). **Countdown or Timer**, **Scoreboard from CSV** and **Lyric Video** work this way.

**Adding a track moves every track below it down one**, so an index you kept from before is wrong afterwards. Look
tracks up by name when you add more than one, as **Photo Slideshow** does:

```javascript
const trackNamed = name => mvp.call("get_project").tracks.find(t => t.name === name).index;
```

### Placing clips without cutting into others

`add_media_to_timeline` puts a whole file down at a time, and anything already there where it lands is cut. Two safe
ways:

- **At the end of the edit**, nothing is in the way: place it at `get_project().duration`, then trim it with
  `edit_event` (`source_in`, `length`). **Edit List from CSV**, **Stream Highlights**, **Photo Slideshow** and
  **Brand Kit** (its outro) do this.
- **Into a gap**: place it past the end first, trim it there, then move it with `edit_event`'s `start`. **Fill the
  Gaps** has a `placeClip` function that does all of it, and drops the clip's sound when its audio track is busy.

A picture and its sound are grouped: `edit_event` on one moves and trims both. To keep only the picture, ungroup them
(`group` with `action: "ungroup"`) and delete the sound.

### Making room

`cut_time` with `action: "insert"` puts empty time into the whole edit at a point, moving everything after it along
(markers too). **Brand Kit** makes room for the intro that way.

### Borrowing the markers

`detect_beats` puts a marker on every beat, and `beat_montage` cuts on markers. **Music Video in One Click** and
**Lyric Video** keep a list of the markers that were there, use the beat markers, then remove all markers
(`remove_markers` with no times) and put the kept ones back. Regions (markers with a length) are never removed unless
asked for.

### Changing a copy, then putting the edit back

`mvp.rollback()` takes back everything the command has changed so far. **Social Cut-downs** reframes the edit for a
vertical frame, queues the render (the Render Queue keeps its own copy of the project), then rolls back:

```javascript
try {
    mvp.call("auto_reframe", { event_ids: ids, portrait: true });
    mvp.call("render_queue", { action: "add", path: path, from: from, to: to });
} finally {
    mvp.rollback();
    mvp.call("set_project_settings", { width: before.width, height: before.height, fps: before.fps });
}
```

The frame size isn't part of undo, so it is set back by hand.

### Sound

**Podcast Clean-up**: `normalize_audio` evens out events, `edit_track` with `duck_under` turns a track down while
another has sound, `cut_time` with `action: "silences"` takes out pauses, and `audio_finishing` with `action:
"apply_gain"` sets the loudness of the whole mix.

### The Live tab

`set_pad` fills a pad with a named chain of effects (each `{type, params}`); **Genre Pads** has five sets of eight.
`live_setlist` replaces the setlist (**Setlist from CSV**). `live_status` reads it all back.

### Random, but the same each time

`Math.random()` can't be given a seed, so **Fill the Gaps** has a few-line `randomFrom(seed)` that gives the same
numbers for the same seed: the user can try another seed for another result, and get the same one back.

## Reference

### plugin.json

| Key | |
|---|---|
| `id` | Required. Lower-case letters, digits, `.`, `-` and `_`. A plugin in your plugins folder with the same id as one that comes with the program replaces it. |
| `name` | Its name (shown when it asks to run). |
| `version`, `author`, `description` | Shown in the menu's tooltip and at the top of its forms. |
| `mvp` | Optional: the oldest version of Mutant Video Producer it works in, e.g. `"1.2"`. |
| `commands` | Required: one or more. Each has a `label` (its menu entry), a `script` (a `.js` file in the plugin's folder), and optionally a `description` (for its form) and `fields`. |

### Fields

Every field has an `id` (how the script reads it: `mvp.args.<id>`), a `label`, a `type` and usually a `default`.

| type | In the form | `mvp.args.<id>` is | Extra keys |
|---|---|---|---|
| `file` | a file box and **Browse...**; the file may be read | its path | `filter` (as in an open dialog: `"Lyrics (*.lrc);;All files (*)"`), `optional` |
| `files` | several files; each may be read | a list of paths | `filter`, `optional` |
| `folder` | a folder; its files may be listed and read | its path | `optional` |
| `save` | a "save as" box; that file may be written | its path | `filter`, `optional` |
| `choice` | a drop-down list | the chosen value | `choices` (the values), `labels` (what the list shows instead) |
| `number` | a number box | a number | `min`, `max` |
| `bool` | a tick box | `true` or `false` | |
| `text` | a line of text | the text | |
| `color` | a colour button | `"#rrggbb"` | |

A `file`, `files`, `folder` or `save` field must be filled in, unless it has `"optional": true` (then an empty one
gives `""`, or `[]` for `files`). The form checks that a chosen file or folder exists before the script runs.

### The mvp object

| | |
|---|---|
| `mvp.args` | What was entered in the form, by field id. |
| `mvp.call(name, args)` | Runs a command and gives back its answer as an object. Throws an `Error` with the reason when it fails (catch it with `try` / `catch` to carry on). See [Plugin Commands](plugin-commands.md). |
| `mvp.readText(path)` | A text file's contents (UTF-8): a file chosen in a `file` or `files` field, or one inside a chosen `folder`. Up to 16 MB. |
| `mvp.listFolder(path, patterns)` | The files in a chosen `folder` (full paths, sorted by name), those matching `patterns` such as `["*.jpg"]`. |
| `mvp.writeText(path, text)` | Writes a text file: only the one a `save` field named. |
| `mvp.rollback()` | Takes back everything the command has changed so far. |
| `mvp.log(text)` | A message; the last one shows in the status bar, and all of them with an error. |
| `mvp.tools()` | Every command `mvp.call` takes, with its `description` and its arguments (`inputSchema`). |
| `mvp.plugin` | The plugin's `id`, `version` and `folder`. |

### Rules and limits

- The script runs once, top to bottom, inside the program, while the window waits. The commands it calls take as long
  as they need (a beat scan, a loudness measure), but the script's own JavaScript is stopped after **20 seconds** of
  running, so a loop that never ends can't hang the program.
- **Not available to plugins**: `new_project`, `open_project`, `undo` and `redo`. A plugin works on the open project,
  in one undo step.
- A script and `plugin.json` can each be up to 1 MB.
- There's no `require`, `import`, `fetch`, timers or file access other than the `mvp` functions above.

## For AI assistants writing plugins

A checklist, so a plugin works the first time:

1. **Use only commands and arguments that exist.** [Plugin Commands](plugin-commands.md) lists every command with every
   argument and its type; `mvp.tools()` gives the same at run time. Don't guess names: an unknown argument is ignored or
   refused.
2. **Get ids from `get_project`**, and call it again after changing things. Event and media ids stay the same; track
   indexes change when a track is added or deleted (look tracks up by name).
3. **Times are seconds** (numbers), from the start of the timeline; an event's `source_in` is seconds into its file.
4. **Effect, transition and preset ids** come from `list_effects` (`kind`: `video`, `audio`, `transition`,
   `generator`, with `search`) and `presets` (`action: "list"`). An unknown effect id is refused.
5. **Never place a file over other events** unless that's the point: append at `duration`, or park past the end, trim
   and move (see [Placing clips](#placing-clips-without-cutting-into-others)).
6. **An event longer than what's left of its file** needs `loop: true` in `edit_event`, or it is shortened.
7. **Check what the user gave**, and `throw new Error("...")` with a sentence that says what to do: the message is
   shown to them as it is.
8. **The JavaScript is Qt's engine**: modern syntax (`const`, `let`, arrow functions, classes, template strings,
   destructuring, spread, `?.`, `??`, `Set`, `Map`, `includes`, `padStart`, `Math.imul`, `JSON`) works. These **don't**:
   `String.prototype.matchAll` (use a `/g` regex with `exec` in a loop), `Array.prototype.flat`,
   `Object.fromEntries`, `async` / `await`, and named regex groups (`(?<name>...)`). There's no `fetch`, `require`,
   `setTimeout` or `import`; `console.log` writes to the program's debug output.
9. **Keep the user's things**: put back markers you borrow, leave locked tracks alone, and use `mvp.rollback()` when
   a change was only needed for a moment.
10. **Test it**: run it in the program on a small project, press Ctrl+Z, run it again with other choices.

A skeleton to start from:

```json
{
  "id": "my-plugin",
  "name": "My Plugin",
  "version": "1.0",
  "author": "",
  "description": "What it does, in a sentence.",
  "commands": [
    { "label": "My Plugin...", "script": "main.js",
      "fields": [ { "id": "seconds", "label": "How long (seconds)", "type": "number", "default": 5, "min": 0.1, "max": 600 } ] }
  ]
}
```

```javascript
// My Plugin: what it does.
const args = mvp.args;
const project = mvp.call("get_project");
if (project.duration <= 0)
    throw new Error("the timeline is empty: put something on it first");

// ... the work, through mvp.call ...

mvp.log("My Plugin: done");
```
