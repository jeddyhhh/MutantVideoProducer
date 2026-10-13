# README screenshot recipe

Run from the repository root after building the Windows application:

```powershell
./scripts/update-readme-screenshots.ps1
```

The recipe builds three editable `.mvp` projects in the ignored
`creative-test/readme-showcase` folder, using `scripts/readme-showcase.py`:

- Orbital Report: native broadcast graphics, a path-following satellite, telemetry chart and captions.
- Night Circuit: native curved paths, reusable car symbols and motion trails.
- Signal Studio: stereo peaks decoded from the bundled tutorial music and styled with the waveform generator.

Screenshots come from the real application's isolated `--screenshot` mode, which
uses fresh settings instead of the user's project, layouts or media library. The
composer capture opens the selected event's actual graphics editor. FFmpeg creates
the four-theme comparison sheet and encodes an editor preview into a short local
video for the Live input. No external artwork or downloaded footage is used.

Review each capture before committing it. Keep the preview capture
`editor-window_PreviewWidget.png` in the working folder only; the README uses the full editor view.
The existing transition and generator reference sheets are produced by the native
self-test commands documented in [Testing](testing.md); the README now focuses
on complete editing workspaces and the newer native graphics tools.
