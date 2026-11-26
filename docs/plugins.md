# Plugins

A plugin adds commands to **Tools > Plugins**. It is a folder holding a `plugin.json` and one or more JavaScript
files. Choosing one of its commands shows a small form (a file, a choice, a number...); then its script changes the
open project through the same commands [AI Control](ai-control.md) gives an assistant: it adds tracks, titles,
effects and markers, cuts, renders and so on. Mutant Video Producer comes with one, **Lyric Video from LRC**.

## Using plugins

- **Tools > Plugins** lists every plugin's commands. Choose one, fill in its form and press **OK**.
- Everything a command does is **one step** in **Edit > Undo**: one Ctrl+Z takes it all away.
- **Tools > Plugins > Open Plugins Folder** opens the folder for your own plugins. To install a plugin, put its folder
  there (unzip it so that `plugin.json` is directly inside its own folder), then open the menu again: it is read each
  time the menu opens, so there is nothing to restart.
- A plugin you added asks before it runs the first time, and again whenever its files change. Run only plugins from
  people you trust: a plugin can change the project, save and render files and do anything else AI Control can.
- If a plugin's folder has a mistake, the menu shows **Plugins That Didn't Load**, which says what is wrong.

A plugin can't read or write files by itself, and it can't go online. It reads only the files you pick in its form,
and everything else it does goes through the program's own commands, which you can undo.

### Lyric Video from LRC

Puts the lines of an `.lrc` file (the usual timed-lyrics format, `[00:12.40] Under the neon rain`) on a new
**Lyrics** track as titles, each until the next line starts. Choose the **Style** (kinetic words, a centred title or a
subtitle) and a **Font**. With **Move each line onto the nearest beat**, it finds the beats of the music on the
timeline and moves each line's start onto the closest one (markers you had are kept). **Shift every line** moves
them all earlier or later, for lyrics timed to another cut of the song.

To try it, put a song on the timeline and pick `example.lrc` from the plugin's folder
(`plugins/Lyric Video` next to the program).

## Writing a plugin

A plugin is a folder:

```
Even Markers/
  plugin.json      its name and its commands, with their forms
  main.js          a command's script
```

### plugin.json

```json
{
  "id": "even-markers",
  "name": "Even Markers",
  "version": "1.0",
  "author": "Your Name",
  "description": "A marker every so many seconds, named Part 1, Part 2...",
  "mvp": "1.1",
  "commands": [
    {
      "label": "Even Markers...",
      "script": "main.js",
      "fields": [
        { "id": "seconds", "label": "Seconds apart", "type": "number", "default": 30, "min": 1, "max": 3600 },
        { "id": "prefix", "label": "Name", "type": "text", "default": "Part" }
      ]
    }
  ]
}
```

- **id**: lower-case letters, digits, `.`, `-` and `_`. A plugin in your folder with the same id as one that comes
  with the program replaces it.
- **mvp** (optional): the oldest version of Mutant Video Producer it works in.
- **commands**: each has a **label** (its menu entry), a **script** (a file in the plugin's folder) and, if it needs
  them, **fields** for its form and a **description** shown at the top of the form.

Each field has an **id** (the name the script reads), a **label**, a **type** and a **default**:

| type | In the form | The script gets |
|---|---|---|
| `file` | a file name and **Browse...** (`filter`: as in an open dialog, e.g. `"Lyrics (*.lrc);;All files (*)"`) | the file's path |
| `choice` | a list: `choices` are the values, `labels` (optional) what the list shows | the chosen value |
| `number` | a number box (`min`, `max`) | a number |
| `bool` | a tick box | `true` or `false` |
| `text` | a line of text | the text |

The form remembers what was entered last time.

### The script

The script is JavaScript (modern JavaScript without modules: `const`, `let`, arrow functions, classes and so on).
It sees one object, `mvp`:

- **`mvp.args`**: the form's values, by field id: `mvp.args.seconds`.
- **`mvp.call(command, arguments)`**: runs one of the program's commands and gives back its answer as an object, for
  example `mvp.call("add_marker", { time: 12.5, name: "Chorus" })`. When a command fails it throws an `Error`
  saying why, which the script can catch with `try` / `catch`.
- **`mvp.readText(path)`**: a text file's contents, for a file picked in a `file` field (no others).
- **`mvp.log(text)`**: a message. The last one shows in the status bar when the command is done. All of them show
  with the error if the script fails.
- **`mvp.tools()`**: the commands `mvp.call` takes, each with its `description` and the arguments it takes
  (`inputSchema`).
- **`mvp.plugin`**: the plugin's `id`, `version` and `folder`.

`throw new Error("...")` stops the command and shows the message. What it changed before stopping stays, as one step
of Edit > Undo.

The commands are the AI Control tools. [What it can do](ai-control.md#what-it-can-do) lists them, and
`mvp.tools()` gives each one's arguments. Times are in seconds; `get_project` describes the project, with the ids
of its media, tracks, events and effects. The program's commands also check their own arguments, and refuse
(throw) with a reason when something is wrong.

A plugin's commands work on the open project. These aren't available to them: `new_project`, `open_project`, `undo`
and `redo`.

```javascript
// Even Markers: a marker every so many seconds, named Part 1, Part 2...
const end = mvp.call("get_project").duration;
if (end <= 0)
    throw new Error("the timeline is empty");
let n = 0;
for (let t = 0; t < end; t += mvp.args.seconds)
    mvp.call("add_marker", { time: t, name: mvp.args.prefix + " " + (++n) });
mvp.log(n + " markers");
```

### Limits

- A script runs inside the program while the window waits. The commands it calls can take as long as they need
  (a render, captions), but the script's own JavaScript is stopped after 20 seconds of running, so a loop that never
  ends can't hang the program.
- A script and `plugin.json` can each be up to 1 MB, and `mvp.readText` reads files of up to 16 MB.
- There is no `require`, `import`, file, network or timer access: what a plugin can do is what `mvp.call` can.

### Sharing a plugin

Zip the plugin's folder. Whoever gets it unzips it into their plugins folder (**Tools > Plugins > Open Plugins
Folder**).

The Lyric Video plugin's own files (`plugins/Lyric Video` next to the program, or
[data/plugins](https://github.com/jeddyhhh/MutantVideoProducer/tree/main/data/plugins) in the source) are a
complete example to start from.
