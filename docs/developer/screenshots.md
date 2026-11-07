# README screenshot recipe

Run from the repository root after building the Windows application:

```powershell
./scripts/update-readme-screenshots.ps1 -Episodes "<folder of the Snowbank episodes>"
```

It writes the README's pictures to `docs/screenshots/readme`, working in the ignored
`creative-test/readme-screenshots` folder. The Editor, Animation, Mixer, Deliver, Render As and theme pictures,
and the six preview frames, come from the Snowbank episodes (*Four Score*, *Whiteout Part 1* and *Get Dry*), a
cartoon series made in Mutant Video Producer; the effects and keyframes pictures use the bundled
`data/examples/effects-and-keyframes.mvp`. The Live pictures play a clip made from the six frames through
`data/examples/live-set.mvp` with its sequence playing (and the sequencer popped out). The Effect Builder pictures
open two of the example effects on *Four Score*'s video, and the effects gallery at the top is
`mvp_selftest fxshowcase <out.png> <columns> "<effect id or node:Name>|<picture>|<label>"...` on the six frames.

Screenshots come from the real application's isolated `--screenshot` mode: fresh settings instead of the user's,
the window kept off the screen, and with `--live-video` only that video as a source (never the computer's screens or
cameras). Its options:

- `--seek <s>`, `--theme <id>`, `--delay <ms>` (before the grab), `--tab <n>` (the media / FX browser tab),
  `--browse <folder>`, `--mode <n>` (0 Editor, 1 Live, 2 Deliver).
- `--fx-editor` shows an event's effects (`--event <id>`, else the first event); `--keyframes` / `--keyframes-graph`
  the Keyframes tab; `--mixer` the Audio Mixer (saved as `<shot>_mixer.png`); `--export-dialog[=search]` Render As
  (`<shot>_export.png`).
- `--trigger effectBuilder --effect-file <file.mvpfx>` opens the Effect Builder on that effect (add
  `--grab NodeEffectEditor` to save its window; `--graph-zoom <z>` shows the graph at that zoom, centred).
- `--trigger <name>` clicks a button or runs an action by object name, or runs a menu command by its words
  (`--trigger "Record Voice-Over"`); `--grab <name>` also saves one widget or dialog by object or class name
  (`<shot>_<name>.png`).

Review each capture before committing it. The pictures in `docs/screenshots` itself (used by the User Guide) and the
transition and generator reference sheets come from the native self-test commands documented in
[Testing](testing.md).
