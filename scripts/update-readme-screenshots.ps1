param(
    # The Snowbank episodes (made in MVP): "<folder>/<name>/<name>.mvp".
    [string]$Episodes = 'G:/MEDIA PROJECTS/MontageProjects/Projects/Generated'
)
$ErrorActionPreference = 'Stop'
# Run from the repository root after building MutantVideoProducer.exe. Writes docs/screenshots/readme.
$exe = Join-Path $PWD 'build/Release/bin/MutantVideoProducer.exe'
$work = 'creative-test/readme-screenshots'
$out = 'docs/screenshots/readme'
New-Item -ItemType Directory -Force $work, "$work/frames", $out | Out-Null
function Episode([string]$name) { "`"$Episodes/$name/$name.mvp`"" }
function Capture([string[]]$CaptureArgs) {
    $p = Start-Process -FilePath $exe -ArgumentList $CaptureArgs -WindowStyle Hidden -PassThru
    $p.WaitForExit()
    if ($p.ExitCode) { throw "Screenshot failed: $CaptureArgs" }
}
$four = Episode '23 Four Score'
$white = Episode '24 Whiteout Part 1'

Capture @('--screenshot', "$work/editor.png", $four, '--seek', '165', '--tab', '0', '--delay', '6000')
Capture @('--screenshot', "$work/composer.png", $four, '--seek', '360', '--fx-editor', '--event', '265', '--trigger', 'composeGraphicsButton', '--grab', 'graphicsComposer', '--delay', '7000')
Capture @('--screenshot', "$work/effects.png", 'data/examples/effects-and-keyframes.mvp', '--seek', '3', '--fx-editor', '--event', '4', '--trigger', 'scopesToggle', '--theme', 'neon', '--delay', '7000')
Capture @('--screenshot', "$work/keyframes.png", 'data/examples/effects-and-keyframes.mvp', '--seek', '5', '--keyframes-graph', '--delay', '5000')
Capture @('--screenshot', "$work/mixer.png", $white, '--seek', '203', '--mixer', '--delay', '6000')
# The Effect Builder: a whole graph, and a close-up of nodes at 1:1 with their GPU / CPU labels.
Capture @('--screenshot', "$work/nodeeditor.png", $four, '--trigger', 'effectBuilder', '--effect-file', 'data/effects/Plasma Possession.mvpfx', '--grab', 'NodeEffectEditor', '--delay', '9000')
Capture @('--screenshot', "$work/nodezoom.png", $four, '--trigger', 'effectBuilder', '--effect-file', 'data/effects/Analogue Circuit Emu/Composite Circuit.mvpfx', '--graph-zoom', '1', '--grab', 'NodeEffectEditor', '--delay', '9000')
Capture @('--screenshot', "$out/deliver.png", $white, '--mode', '2', '--delay', '7000')
Capture @('--screenshot', "$work/export.png", $four, '--export-dialog=YouTube', '--delay', '5000')
foreach ($theme in @('neon', 'nightcity', 'synthwave', 'studio')) {
    Capture @('--screenshot', "$work/theme-$theme.png", $four, '--seek', '361', '--theme', $theme, '--delay', '6000')
}
# Six preview frames: the "made in MVP" strip, and (as a clip) the Live tab's input.
$frames = @(@('23 Four Score', 361), @('23 Four Score', 300), @('24 Whiteout Part 1', 207), @('24 Whiteout Part 1', 426.5), @('22 Get Dry', 150), @('24 Whiteout Part 1', 538.5))
for ($i = 0; $i -lt $frames.Count; $i++) {
    Capture @('--screenshot', "$work/frames/p$($i + 1).png", (Episode $frames[$i][0]), '--seek', "$($frames[$i][1])", '--grab', 'PreviewWidget', '--delay', '7000')
}
python -c @"
from PIL import Image
w = '$work'
for i in range(1, 7):
    im = Image.open(f'{w}/frames/p{i}_PreviewWidget.png'); fw, fh = im.size; pw = int(fh * 16 / 9); x = (fw - pw) // 2
    im.crop((x, 0, x + pw, fh)).resize((1280, 720)).convert('RGB').save(f'{w}/frames/c{i}.png')
sheet = Image.new('RGB', (640 * 3 + 16, 360 * 2 + 8), (18, 18, 24))
for k in range(6):
    sheet.paste(Image.open(f'{w}/frames/c{k + 1}.png').resize((640, 360)), ((k % 3) * 648, (k // 3) * 368))
sheet.save('$out/made-in-mvp.jpg', quality=88, optimize=True)
themes = Image.new('RGB', (1600, 1000))
for k, t in enumerate(['neon', 'nightcity', 'synthwave', 'studio']):
    themes.paste(Image.open(f'{w}/theme-{t}.png').resize((800, 500)), ((k % 2) * 800, (k // 2) * 500))
themes.save('$out/themes.png')
"@
if ($LASTEXITCODE) { throw 'Frame sheets failed' }
& deps/ffmpeg/bin/ffmpeg.exe -y -v error -framerate 1/2 -i "$work/frames/c%d.png" -i data/examples/media/music.m4a -t 12 -vf 'fps=30,format=yuv420p' -c:v libx264 -c:a aac -shortest "$work/snowbank-live.mp4"
if ($LASTEXITCODE) { throw 'Live input clip failed' }
# (--live-video uses only that clip: never this computer's screens or cameras)
# (the example's sequence playing: Loop on; then the sequencer in its own window)
Capture @('--screenshot', "$work/live.png", 'data/examples/live-set.mvp', '--live-video', "$work/snowbank-live.mp4", '--trigger', 'liveSequenceOn', '--theme', 'neon', '--delay', '7000')
Capture @('--screenshot', "$work/liveseq.png", 'data/examples/live-set.mvp', '--live-video', "$work/snowbank-live.mp4", '--trigger', 'liveSequenceOn', '--trigger', 'liveSequencePopOut', '--grab', 'liveSequencerWindow', '--theme', 'neon', '--delay', '7000')
python -c "from PIL import Image; Image.open('$work/live.png').convert('RGB').save('$out/live.jpg', quality=88, optimize=True); Image.open('$work/nodezoom_NodeEffectEditor.png').crop((376, 226, 1190, 760)).save('$out/node-graph.png')"
# Nine effects on the frames (the Node Editor examples need the effects folder next to the program).
$f = "$work/frames"
& build/Release/bin/mvp_selftest.exe fxshowcase "$work/fxsheet.png" 3 `
    "node:Multipath Ghosts|$f/c1.png|Analogue Circuit Emu: Multipath Ghosts" "node:Hum Bars|$f/c2.png|Analogue Circuit Emu: Hum Bars" `
    "vg_gb_glitchcity|$f/c3.png|Broken VGS: Game Boy Glitch City" "vg_nes_palette|$f/c4.png|Broken VGS: NES Palette Corruption" `
    "node:Plasma Possession|$f/c5.png|Node Editor: Plasma Possession" "node:Neon Plasma Title|$f/c2.png|Node Editor: Neon Plasma Title" `
    "node:Kaleido Transmission|$f/c3.png|Analogue Circuit Emu: Kaleido Transmission" "node:Kaleido Feedback|$f/c4.png|Node Editor: Kaleido Feedback" `
    "node:Thermal Uplink|$f/c1.png|Analogue Circuit Emu: Thermal Uplink"
if ($LASTEXITCODE) { throw 'Effects gallery failed' }
python -c "from PIL import Image; Image.open('$work/fxsheet.png').convert('RGB').save('$out/effects-gallery.jpg', quality=86, optimize=True)"

Copy-Item "$work/editor.png" "$out/editor.png"
Copy-Item "$work/composer_graphicsComposer.png" "$out/animation-composer.png"
Copy-Item "$work/effects.png" "$out/effects-scopes.png"
Copy-Item "$work/keyframes.png" "$out/keyframes.png"
Copy-Item "$work/mixer_mixer.png" "$out/mixer.png"
Copy-Item "$work/nodeeditor_NodeEffectEditor.png" "$out/node-editor.png"
Copy-Item "$work/liveseq_liveSequencerWindow.png" "$out/sequencer.png"
Copy-Item "$work/export_export.png" "$out/render-as.png"
