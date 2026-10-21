$ErrorActionPreference = 'Stop'
# Run from the repository root after building MutantVideoProducer.exe.
python scripts/readme-showcase.py
if ($LASTEXITCODE) { throw 'Showcase project generation failed' }
$producerExe = Join-Path $PWD 'build/Release/bin/MutantVideoProducer.exe'
$showcase = 'creative-test/readme-showcase'
function Capture([string[]] $CaptureArgs) {
    $capture = Start-Process -FilePath $producerExe -ArgumentList $CaptureArgs -WindowStyle Hidden -PassThru
    $capture.WaitForExit()
    if ($capture.ExitCode) { throw "Screenshot failed: $CaptureArgs" }
}
# Quote project paths because the native argument list is passed as one command line.
Capture @('--screenshot',"$showcase/editor-window.png","`"$showcase/Orbital Report.mvp`"",'--seek','7','--theme','neon','--tab','6','--browse','Graphics','--grab','PreviewWidget','--delay','3500')
Copy-Item -LiteralPath "$showcase/editor-window.png" -Destination 'docs/screenshots/editor.png'
Capture @('--screenshot',"$showcase/composer-window.png","`"$showcase/Night Circuit.mvp`"",'--seek','7','--theme','nightcity','--fx-editor','--trigger','composeGraphicsButton','--grab','graphicsComposer','--delay','4000')
Copy-Item -LiteralPath "$showcase/composer-window_graphicsComposer.png" -Destination 'docs/screenshots/graphics-composer.png'
Capture @('--screenshot',"$showcase/waveform-window.png","`"$showcase/Signal Studio.mvp`"",'--seek','7','--theme','nightcity','--fx-editor','--delay','3500')
Copy-Item -LiteralPath "$showcase/waveform-window.png" -Destination 'docs/screenshots/waveform.png'
Capture @('--screenshot','docs/screenshots/effects-scopes.png','data/examples/effects-and-keyframes.mvp','--seek','5','--theme','neon','--fx-editor','--trigger','scopesToggle','--delay','3000')
foreach ($theme in @('neon','nightcity','synthwave','studio')) {
    Capture @('--screenshot',"$showcase/theme-$theme.png","`"$showcase/Orbital Report.mvp`"",'--seek','7','--theme',$theme,'--delay','3000')
}
& deps/ffmpeg/bin/ffmpeg.exe -y -v error -i "$showcase/theme-neon.png" -i "$showcase/theme-nightcity.png" -i "$showcase/theme-synthwave.png" -i "$showcase/theme-studio.png" -filter_complex '[0:v]scale=800:500[a];[1:v]scale=800:500[b];[2:v]scale=800:500[c];[3:v]scale=800:500[d];[a][b][c][d]xstack=inputs=4:layout=0_0|800_0|0_500|800_500[v]' -map '[v]' -frames:v 1 docs/screenshots/themes.png
if ($LASTEXITCODE) { throw 'Theme sheet failed' }
# The Live input is a native editor preview, encoded to a short video for capture.
& deps/ffmpeg/bin/ffmpeg.exe -y -v error -loop 1 -i "$showcase/editor-window_PreviewWidget.png" -i data/examples/media/music.m4a -t 10 -vf 'crop=ih*16/9:ih:(iw-ih*16/9)/2:0,scale=1280:720,setsar=1' -c:v libx264 -pix_fmt yuv420p -c:a aac "$showcase/Orbital-Report.mp4"
if ($LASTEXITCODE) { throw 'Live demonstration source failed' }
Capture @('--screenshot','docs/screenshots/live.png','data/examples/live-set.mvp','--live-video',"$showcase/Orbital-Report.mp4",'--live-action','pad:0','--theme','neon','--delay','4000')
