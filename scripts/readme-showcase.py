"""Build native, editable projects used by the README screenshot recipe.

Run from the repository root. Requires FFmpeg in deps/ffmpeg/bin, but no
downloaded footage or external artwork. The audio envelope comes from the
bundled tutorial music; all visual artwork is composed by the editor.
"""
from pathlib import Path
import array
import json
import math
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'creative-test/readme-showcase'
OUT.mkdir(parents=True, exist_ok=True)
MUSIC = ROOT / 'data/examples/media/music.m4a'
pcm = subprocess.check_output([str(ROOT / 'deps/ffmpeg/bin/ffmpeg.exe'), '-v', 'error',
                              '-i', str(MUSIC), '-t', '24', '-ac', '2', '-ar', '8000',
                              '-f', 'f32le', '-'])
samples = array.array('f', pcm)
frames = len(samples) // 2
peaks = [[max(abs(samples[2*k+c]) for k in range(i*frames//512, (i+1)*frames//512))
          for i in range(512)] for c in range(2)]


def rect(name, x, y, w, h, fill, **extra):
    return dict(type='rect', id=name, name=name.replace('_', ' '), x=x, y=y,
                width=w, height=h, fill=fill, **extra)


def text(name, words, x, y, w, h, size=.035, fill='#edfaff', **extra):
    return dict(type='text', id=name, name=name.replace('_', ' '), text=words,
                x=x, y=y, width=w, height=h, font='Segoe UI', font_size=size,
                fill=fill, **extra)


def keys(a, b, duration=24):
    return {'keyframes': [[0, a, 'ease_in_out'], [duration, b]]}


def scene(layers, **extra):
    return dict(version=2, layers=layers, **extra)


def clip(cid, name, generator, length=24, start=0):
    return dict(id=cid, name=name, media=0, stream=-1, start=str(int(start*1e6)),
                length=str(int(length*1e6)), sourceIn='0', fadeIn='0', fadeOut='0',
                gain=1, group=0, muted=False, generator=generator)


def graphics(cid, name, data, **kw):
    return clip(cid, name, dict(type='graphics_overlay', params={'opacity': {'v': 1}}, data=data), **kw)


def track(tid, name, clips, kind='video'):
    return dict(id=tid, name=name, type=kind, level=1, pan=0, muted=False, solo=False, clips=clips)


def save(name, tracks):
    data = dict(format='mutant-video-producer-project', version=1, nextId=200,
                settings=dict(width=1920, height=1080, fpsNum=30, fpsDen=1, sampleRate=48000),
                media=[dict(id=1, path=str(MUSIC))], tracks=tracks, outputEffects=[], audioMasterEffects=[])
    (OUT / (name+'.mvp')).write_text(json.dumps(data, indent=2), encoding='utf-8')


navy = '#071322'
teal = '#62f2ca'
blue = '#53a8ff'
orange = '#ffb563'
background = [rect('background', 0, 0, 1, 1, navy)]
for i in range(15):
    background.append(rect('grid_v_'+str(i), .02+i*.07, .10, .0007, .72, '#142d42'))
for i in range(10):
    background.append(rect('grid_h_'+str(i), .02, .10+i*.08, .96, .001, '#142d42'))
header = [rect('top_rule', .035, .035, .93, .005, teal),
          text('network', 'MUTANT / SCIENCE DESK', .035, .06, .65, .06, .028, teal, bold=True),
          text('edition', 'ORBITAL REPORT  /  07', .68, .06, .285, .06, .021, align='right'),
          text('headline', 'THE NEXT FRONTIER', .035, .13, .94, .10, .072, bold=True),
          text('deck', 'Native graphics. Real audio. One editable timeline.', .035, .25, .93, .055, .027, '#9ab2cc')]
background = [dict(type='group', id='midnight_grid', name='Midnight grid', x=0, y=0,
                   width=1, height=1, children=background)]
orbit = [dict(type='ellipse', id='planet', name='Planet / radial gradient', x=.37, y=.36,
              width=.25, height=.43, fill=blue,
              gradient=dict(type='radial', stops=[[0, '#57d5f3'], [1, '#133e78']], x1=.3, y1=.3, radius=.8)),
         dict(type='path', id='orbit', name='Curved orbital path', x=.28, y=.35, width=.45, height=.46,
              commands=[['M', 1, .5], ['C', 1, -.15, 0, -.15, 0, .5],
                        ['C', 0, 1.15, 1, 1.15, 1, .5], ['Z']],
              fill='#00000000', stroke=teal, stroke_width=.003, stroke_dash=[4, 3]),
         dict(type='ellipse', id='satellite', name='Satellite / path follower', width=.018, height=.032,
              fill=orange, follow=dict(path='orbit', progress=keys(0, 1), orient=True, loop=True),
              trail=dict(samples=6, duration=.4, opacity=.35)),
         text('planet_name', 'AEON / 04', .37, .52, .25, .055, .032, bold=True, align='center'),
         text('orbit_label', 'ANIMATED ORBIT', .34, .77, .38, .045, .023, teal, align='center')]
panels = [rect('left_panel', .035, .36, .22, .44, '#102639', radius=.03),
          text('mission', 'MISSION\nCONTROL', .055, .39, .18, .12, .046, bold=True),
          text('mission_stats', 'ALTITUDE\n480 km\n\nSIGNAL\n98.4%', .055, .53, .18, .24, .025, teal),
          rect('right_panel', .76, .36, .205, .44, '#102639', radius=.03),
          text('data_title', 'TELEMETRY', .777, .39, .17, .05, .026, orange, bold=True),
          dict(type='chart', id='telemetry', name='Editable data chart', x=.775, y=.47, width=.17,
               height=.26, style='bar', maximum=100, font_size=.022, text_color='#edfaff',
               data=[dict(label='A', value=88, color=teal), dict(label='B', value=64, color=blue),
                     dict(label='C', value=96, color=orange)])]
caption = json.loads((ROOT / 'data/examples/audio-mixing.mvp').read_text(encoding='utf-8'))['tracks'][0]['clips'][0]['generator']
caption['params']['text'] = {'v': 0, 'text': 'A new perspective, built entirely inside Mutant Video Producer.'}
caption['params']['y'] = {'v': .945}
caption['params']['size'] = {'v': 2.7}
caption['params']['animIn'] = {'v': 0}
caption['params']['animOut'] = {'v': 0}
audio = dict(id=90, name='Tutorial music', media=1, stream=0, start='0', length='24000000',
             sourceIn='0', fadeIn='500000', fadeOut='1000000', gain=.6, group=0, muted=False)
save('Orbital Report', [track(10, 'Captions', [clip(11, 'Opening caption', caption, length=12),
                                            clip(12, 'Closing caption', caption, start=12, length=12)]),
                       track(20, 'Broadcast graphics', [graphics(21, 'Mission control / editable scene', scene(background+header+orbit+panels))]),
                       track(40, 'Music', [audio], 'audio')])
wave = dict(type='audio_waveform', params={k: {'v': v} for k, v in
            dict(x=.06, y=.43, width=.88, height=.28, amplitude=1, thickness=2,
                 spacing=4, color=int('62f2ca', 16), played_color=int('ffb563', 16),
                 background=int('0b2034', 16), background_opacity=.9, stereo=1,
                 normalize=1, progress=1, scroll=0, window=10, rate=1, offset=0, style=0).items()},
            data=dict(version=1, duration=frames/8000, left=peaks[0], right=peaks[1]))
save('Signal Studio', [track(10, 'Stereo waveform', [clip(11, 'Real audio / stereo peaks', wave)]),
                       track(20, 'Studio graphics', [graphics(21, 'Signal studio', scene(background+[
                           text('title', 'SIGNAL / STUDIO', .06, .13, .9, .12, .075, bold=True),
                           text('subtitle', 'THE SHAPE OF SOUND', .06, .28, .88, .07, .038, teal),
                           text('footer', 'STEREO ENVELOPE  /  EDITABLE COLOURS  /  PLAYBACK PROGRESS',
                                .06, .79, .88, .06, .023, '#9ab2cc')]))]), track(30, 'Music', [audio], 'audio')])
route = [['M', .12, .62], ['C', -.05, .25, .15, -.1, .46, .08],
         ['C', .65, .2, .93, -.05, .95, .32], ['C', 1.06, .62, .7, .97, .52, .77],
         ['C', .38, .55, .3, .93, .12, .62], ['Z']]
race = background+[
    text('race_title', 'NIGHT CIRCUIT', .05, .04, .68, .10, .072, teal, bold=True),
    text('race_subtitle', 'CURVED PATHS / REUSABLE CARS / MOTION TRAILS', .05, .16, .89, .05, .025),
    dict(type='path', id='road', name='Track surface', x=.10, y=.27, width=.8, height=.56,
         commands=route, fill='#00000000', stroke='#344459', stroke_width=.08),
    dict(type='path', id='racing_line', name='Curved racing line', x=.10, y=.27, width=.8, height=.56,
         commands=route, fill='#00000000', stroke=teal, stroke_width=.004, stroke_dash=[3, 5]),
    text('race_footer', 'A NATIVE 2D SCENE  /  THREE INDEPENDENT PATH FOLLOWERS', .05, .9, .9, .05, .025, '#9ab2cc')]
car = [rect('body', 0, .15, 1, .7, {'variable': 'paint'}, radius=.15),
       rect('glass', .34, .22, .30, .56, '#061323', radius=.12),
       rect('stripe', .72, .3, .2, .4, '#ffffff'),
       rect('wheel_a', .18, 0, .18, .2, '#000000'), rect('wheel_b', .68, 0, .18, .2, '#000000'),
       rect('wheel_c', .18, .8, .18, .2, '#000000'), rect('wheel_d', .68, .8, .18, .2, '#000000')]
for i, color in enumerate([teal, orange, blue]):
    race.append(dict(type='instance', id='car_'+str(i), name='Car '+str(i+1), symbol='car', width=.04,
                     height=.045, variables=dict(paint=color),
                     follow=dict(path='racing_line', progress=keys(i*.2, i*.2+2), loop=True, orient=True),
                     trail=dict(samples=6, duration=.4, opacity=.4)))
save('Night Circuit', [track(10, 'Race scene', [graphics(11, 'Cars / curves / symbols',
                       scene(race, symbols=dict(car=dict(layers=car, variables=dict(paint=teal)))))]),
                      track(20, 'Music', [audio], 'audio')])
print('Created Orbital Report, Signal Studio and Night Circuit in', OUT)
