"""Builds data/sfx: the built-in sound effects library. Synthesised sounds (numpy, then FFmpeg filters) plus CC0 sounds
from Kenney (kenney.nl), OpenGameArt and the U.S. Fish & Wildlife Service, all Ogg Vorbis, with CREDITS.md.

python scripts/make-sound-library.py <sources>
<sources> holds Kenney's packs unzipped (kenney_interface-sounds, kenney_ui-audio, kenney_digital-audio,
kenney_sci-fi-sounds, kenney_impact-sounds, kenney_rpg-audio, kenney_casino-audio), group_giggling.ogg and
Well_Done_CCBY3.ogg from OpenGameArt, and bear/ogg/bear_01.ogg (OpenGameArt's U.S. Fish & Wildlife bear growls)."""
import os, shutil, subprocess, sys, tempfile, wave
import numpy as np

SR = 44100
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "data", "sfx")
FF = shutil.which("ffmpeg") or os.path.join(ROOT, "deps", "ffmpeg", "bin", "ffmpeg.exe")
TMP = tempfile.mkdtemp()
DL = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
OLD = DL
rng = np.random.default_rng(7)
credits = {}  # category -> [(name, source)]


def t_(d):
    return np.arange(int(d * SR)) / SR


def phase(freq):  # integrates a frequency curve into a phase
    return 2 * np.pi * np.cumsum(freq) / SR


def env(n, attack=0.005, decay=None, d=None):
    t = np.arange(n) / SR
    e = np.minimum(1, t / max(attack, 1e-4))
    if decay:
        e *= np.exp(-t / decay)
    return e


def harmonics(freq, count=8, tilt=1.0):
    p = phase(freq)
    return sum(np.sin(k * p) / k ** tilt for k in range(1, count + 1))


def place(total, parts):
    out = np.zeros(int(total * SR))
    for at, s in parts:
        i = int(at * SR)
        s = s[: len(out) - i]
        out[i:i + len(s)] += s
    return out


def noise(d, color="white"):
    n = rng.standard_normal(int(d * SR))
    if color == "pink":
        f = np.fft.rfft(n)
        f /= np.sqrt(np.maximum(np.arange(len(f)), 1))
        n = np.fft.irfft(f, len(n))
    return n / np.max(np.abs(n))


def write(category, name, x, af="", source="Synthesised for Mutant Video Producer (CC0)"):
    os.makedirs(f"{OUT}/{category}", exist_ok=True)
    x = np.asarray(x, dtype=float)
    x = x / (np.max(np.abs(x)) + 1e-9) * 0.89
    fade = min(len(x), int(0.01 * SR))
    x[-fade:] *= np.linspace(1, 0, fade)
    raw = f"{TMP}/{category}-{name}.wav"
    with wave.open(raw, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((x * 32767).astype("<i2").tobytes())
    encode(raw, category, name, af)
    credits.setdefault(category, []).append((name, source))


def encode(src, category, name, af=""):
    os.makedirs(f"{OUT}/{category}", exist_ok=True)
    chain = ",".join(f for f in (af, "loudnorm=I=-16:TP=-1.5:LRA=11", "aresample=44100") if f)
    subprocess.run([FF, "-v", "error", "-y", "-i", src, "-af", chain, "-c:a", "libvorbis", "-q:a", "5", f"{OUT}/{category}/{name}.ogg"],
                   check=True)


def copy(category, name, path, source, af=""):
    encode(path, category, name, af)
    credits.setdefault(category, []).append((name, source))


KENNEY = "Kenney (kenney.nl), {pack}, CC0"


def kenney(category, name, pack, file, af=""):
    roots = [f"{DL}/kenney_{pack}", f"{OLD}/kenney_{pack}"]
    for root in roots:
        for dirpath, _, files in os.walk(root):
            if file in files:
                return copy(category, name, f"{dirpath}/{file}", KENNEY.format(pack=pack.replace("-", " ").title()), af)
    sys.exit(f"missing {pack}/{file}")


# ---- synthesised ----
def bell(f, d=1.5, decay=0.4):
    t = t_(d)
    return sum(a * np.sin(2 * np.pi * f * r * t) * np.exp(-t / (decay / r ** 0.5)) for r, a in ((1, 1), (2.76, .45), (5.4, .25), (8.9, .12)))


def tom(f0=180, f1=95, d=0.35):
    t = t_(d)
    return np.sin(phase(f1 + (f0 - f1) * np.exp(-t * 18))) * np.exp(-t * 9)


def cymbal(d=1.4, decay=0.45):
    return noise(d) * np.exp(-t_(d) / decay)


def brass(notes, total, tilt=1.1, vib=0.0):
    parts = []
    for at, f, d in notes:
        t = t_(d)
        freq = f * (1 + vib * np.sin(2 * np.pi * 5.5 * t) * np.minimum(1, t / 0.3))
        a = np.minimum(1, t / 0.03) * np.minimum(1, (d - t) / 0.08)
        parts.append((at, harmonics(freq, 10, tilt) * a))
    return place(total, parts)


def synth():
    T = "Transitions"
    d = 0.7
    write(T, "Whoosh", noise(d, "pink") * np.sin(np.pi * t_(d) / d) ** 3, "bandpass=f=900:width_type=o:w=2.5,flanger=delay=4:depth=6:speed=1.2")
    d = 1.4
    write(T, "Whoosh Slow", noise(d, "pink") * np.sin(np.pi * t_(d) / d) ** 2, "bandpass=f=600:width_type=o:w=2,flanger=delay=6:depth=8:speed=0.5")
    one = noise(0.35, "pink") * np.sin(np.pi * t_(0.35) / 0.35) ** 3
    write(T, "Swoosh Double", place(0.8, [(0, one), (0.33, one)]), "bandpass=f=1400:width_type=o:w=2")
    d = 3.0
    t = t_(d)
    k = 12
    rise = np.sin(phase(180 * k ** (t / d))) * (t / d) ** 2 + 0.6 * noise(d, "pink") * (t / d) ** 3
    write(T, "Riser", rise, "highpass=f=120,aecho=0.8:0.6:60:0.3")
    d = 2.2
    write(T, "Reverse Swell", noise(d) * (t_(d) / d) ** 4, "highpass=f=2500,aecho=0.8:0.5:40:0.35")
    d = 0.5
    t = t_(d)
    write(T, "Zoom In", np.sin(phase(300 + 2200 * (t / d) ** 2)) * np.sin(np.pi * t / d), "aecho=0.8:0.4:30:0.3")
    write(T, "Zoom Out", np.sin(phase(2500 - 2200 * (t / d) ** 0.5)) * np.sin(np.pi * t / d), "aecho=0.8:0.4:30:0.3")

    H = "Hits and Booms"
    d = 2.6
    t = t_(d)
    boom = np.sin(phase(42 + 75 * np.exp(-t * 5))) * np.exp(-t * 1.6) * np.minimum(1, t / 0.004)
    write(H, "Meme Boom", np.tanh(3 * boom), "aecho=0.8:0.7:90:0.25,lowpass=f=1800")
    d = 3.2
    t = t_(d)
    drop = np.sin(phase(30 + 140 * np.exp(-t * 1.4))) * np.exp(-t * 0.9) * np.minimum(1, t / 0.01)
    write(H, "Bass Drop", np.tanh(2.2 * drop), "lowpass=f=900")
    d = 3.0
    t = t_(d)
    hit = np.tanh(3 * np.sin(phase(38 + 60 * np.exp(-t * 6))) * np.exp(-t * 1.2)) + 0.5 * cymbal(d, 0.5)
    write(H, "Dramatic Hit", hit, "aecho=0.8:0.8:120|240:0.35|0.2")
    d = 0.25
    t = t_(d)
    write(H, "Slap", noise(d) * np.exp(-t * 40) + 0.6 * np.sin(phase(np.full(len(t), 220))) * np.exp(-t * 30), "highpass=f=400")

    C = "Comedy"
    d = 0.9
    t = t_(d)
    write(C, "Boing", np.sin(phase(260 * (1 + 0.45 * np.exp(-t * 3) * np.sin(2 * np.pi * 11 * t)))) * np.exp(-t * 3.5) * np.minimum(1, t / .005))
    notes = [(0, 233.1, 0.42), (0.47, 220.0, 0.42), (0.94, 207.7, 0.42), (1.41, 196.0, 1.3)]
    write(C, "Sad Trombone", brass(notes[:3], 2.8, 1.3) + brass(notes[3:], 2.8, 1.3, vib=0.025),
          "lowpass=f=2200,tremolo=f=5.5:d=0.25,aecho=0.8:0.5:50:0.3")
    write(C, "Rimshot", place(1.8, [(0, tom(200, 120)), (0.17, tom(150, 85)), (0.42, 0.7 * cymbal(1.35, 0.4))]), "highpass=f=60")
    d = 0.9
    t = t_(d)
    write(C, "Slide Whistle Up", np.sin(phase((500 + 1700 * (t / d) ** 1.3) * (1 + .01 * np.sin(2 * np.pi * 7 * t)))) * np.sin(np.pi * t / d) ** .5)
    write(C, "Slide Whistle Down", np.sin(phase((2200 - 1700 * (t / d) ** .8) * (1 + .01 * np.sin(2 * np.pi * 7 * t)))) * np.sin(np.pi * t / d) ** .5)
    d = 0.14
    t = t_(d)
    write(C, "Cartoon Pop", np.sin(phase(350 + 1500 * (t / d))) * np.exp(-t * 28) * np.minimum(1, t / .002))
    honk = brass([(0, 415, 0.28), (0.36, 415, 0.45)], 0.9, 0.8)
    write(C, "Clown Horn", np.tanh(2 * honk), "bandpass=f=900:width_type=o:w=2.5")
    blast = lambda d: harmonics(np.full(int(d * SR), 233.0), 16, 0.7) + harmonics(np.full(int(d * SR), 277.2), 16, 0.7)
    horn = place(1.65, [(0, blast(0.16)), (0.22, blast(0.16)), (0.44, blast(1.15))])
    write(C, "Air Horn", np.tanh(1.5 * horn / 3), "highpass=f=200,aecho=0.8:0.6:70:0.3")
    chirp = lambda: np.sin(2 * np.pi * 4600 * t_(0.12)) * (np.sin(2 * np.pi * 33 * t_(0.12)) > 0) * np.hanning(int(0.12 * SR))
    crick = [(i * 0.62 + (0.3 if j else 0) + rng.uniform(0, .03), chirp() * (0.6 if j else 1)) for i in range(7) for j in (0, 1)]
    write(C, "Crickets", place(4.6, crick) + 0.03 * noise(4.6, "pink"), "aecho=0.8:0.5:80:0.25")
    write(C, "Dun Dun Dunnn", brass([(0, 146.8, 0.32), (0.4, 146.8, 0.32), (0.8, 116.5, 2.0)], 3.0, 1.0, vib=0.006)
          + place(3.0, [(0, tom(90, 50, .6)), (0.4, tom(90, 50, .6)), (0.8, tom(80, 40, 1.5))]), "lowpass=f=3000,aecho=0.8:0.7:90|180:0.3|0.2")
    d = 0.85
    t = t_(d)
    buzz = np.sign(np.sin(2 * np.pi * 140 * t)) + np.sign(np.sin(2 * np.pi * 147 * t))
    write(C, "Wrong Buzzer", buzz * np.minimum(1, (d - t) / .05), "lowpass=f=2500")
    d = 2.6
    t = t_(d)
    roll = sum(noise(.07) * np.exp(-t_(.07) * 50) for _ in range(1))
    hits = [(i / 26, roll * (0.25 + 0.75 * (i / 26 / d) ** 1.5)) for i in range(int(26 * 2.3))]
    write(C, "Drum Roll", place(4.0, hits + [(2.35, tom(160, 90, .5)), (2.35, cymbal(1.6, .55))]), "bandpass=f=2500:width_type=o:w=3")
    write(C, "Ta-Da", brass([(0, 261.6, .14), (.14, 329.6, .14), (.28, 392.0, .14), (.42, 523.3, 1.3)], 1.9, 1.0, vib=.008)
          + brass([(.42, 392.0, 1.3), (.42, 329.6, 1.3)], 1.9, 1.0) * .6, "aecho=0.8:0.6:60:0.3")

    N = "Notifications and UI"
    write(N, "Ding", bell(1318.5, 1.6, .45), "aecho=0.8:0.4:40:0.2")
    write(N, "Notification", place(1.2, [(0, bell(1046.5, .8, .2)), (.12, bell(1568, 1.0, .3))]))
    write(N, "Correct Answer", place(1.4, [(0, bell(784, 1, .3)), (.1, bell(1046.5, 1, .3)), (.2, bell(1568, 1.2, .45))]))
    clicks = lambda: noise(.012) * np.exp(-t_(.012) * 500)
    shutter = place(0.3, [(0, clicks()), (0.004, 0.5 * noise(.05) * np.exp(-t_(.05) * 90)), (0.11, clicks() * .8),
                          (0.115, 0.4 * noise(.06) * np.exp(-t_(.06) * 70))])
    write(N, "Camera Shutter", shutter, "highpass=f=900,bandpass=f=3500:width_type=o:w=3")
    notes = [523.3, 587.3, 659.3, 784, 880, 1046.5, 1174.7, 1318.5, 1568, 1760, 2093]
    write(N, "Sparkle", place(1.6, [(i * .045, bell(f, .6, .12) * .6) for i, f in enumerate(notes)]), "aecho=0.8:0.6:45|90:0.4|0.25")
    write(N, "Cha-Ching", place(1.6, [(0, 0.8 * noise(.09) * np.exp(-t_(.09) * 30)), (.12, bell(2093, 1.4, .4)), (.13, bell(2637, 1.4, .35))]),
          "highpass=f=300")

    E = "Everyday"
    lub = lambda f, a: np.sin(phase(np.full(int(.16 * SR), f))) * np.exp(-t_(.16) * 24) * a
    write(E, "Heartbeat", place(3.0, [(i * .95 + o, lub(55, a)) for i in range(3) for o, a in ((0, 1), (.24, .7))]), "lowpass=f=250")


def packs():
    T, H, C, N, E = "Transitions", "Hits and Booms", "Comedy", "Notifications and UI", "Everyday"
    kenney(T, "Laser Swipe", "digital-audio", "phaserUp3.ogg")
    kenney(H, "Punch", "impact-sounds", "impactPunch_heavy_000.ogg")
    kenney(H, "Thud", "impact-sounds", "impactSoft_heavy_000.ogg")
    kenney(H, "Metal Clang", "impact-sounds", "impactMetal_heavy_000.ogg")
    kenney(H, "Glass Hit", "impact-sounds", "impactGlass_heavy_000.ogg")
    kenney(H, "Bell Hit", "impact-sounds", "impactBell_heavy_000.ogg")
    kenney(H, "Explosion", "sci-fi-sounds", "explosionCrunch_000.ogg")
    kenney(H, "Deep Explosion", "sci-fi-sounds", "lowFrequency_explosion_000.ogg")
    copy(C, "Giggling", f"{DL}/group_giggling.ogg", "\"Group Giggling\" by Nocturnal_Vanguard, OpenGameArt.org, CC0")
    copy(C, "Applause", f"{DL}/Well_Done_CCBY3.ogg", "\"Well Done\" by qubodup, OpenGameArt.org, CC0 (since 2024-10-05)")
    kenney(N, "Click", "interface-sounds", "click_001.ogg")
    kenney(N, "Tap", "ui-audio", "click1.ogg")
    kenney(N, "Toggle", "interface-sounds", "toggle_001.ogg")
    kenney(N, "Pop", "interface-sounds", "drop_002.ogg")
    kenney(N, "Error", "interface-sounds", "error_006.ogg")
    kenney(N, "Confirm", "interface-sounds", "confirmation_001.ogg")
    kenney(N, "Question", "interface-sounds", "question_001.ogg")
    kenney(N, "Level Up", "digital-audio", "powerUp2.ogg")
    kenney(N, "Power Up", "digital-audio", "powerUp7.ogg")
    kenney(N, "Laser", "digital-audio", "laser1.ogg")
    kenney(N, "Zap", "digital-audio", "zap1.ogg")
    kenney(N, "Three Tone", "digital-audio", "threeTone1.ogg")
    arrange(E, "Footsteps", "impact-sounds", [f"footstep_concrete_00{i % 5}.ogg" for i in range(6)], [i * 0.48 for i in range(6)])
    arrange(E, "Footsteps Snow", "impact-sounds", [f"footstep_snow_00{i % 5}.ogg" for i in range(6)], [i * 0.52 for i in range(6)])
    kenney(E, "Door Open", "rpg-audio", "doorOpen_1.ogg")
    kenney(E, "Door Close", "rpg-audio", "doorClose_1.ogg")
    kenney(E, "Page Flip", "rpg-audio", "bookFlip1.ogg")
    kenney(E, "Coins", "rpg-audio", "handleCoins.ogg")
    kenney(E, "Dice Roll", "casino-audio", "dice-throw-1.ogg")
    kenney(E, "Card Shuffle", "casino-audio", "card-shuffle.ogg")
    kenney(E, "Knife", "rpg-audio", "knifeSlice.ogg")
    copy(E, "Bear Growl", f"{OLD}/bear/ogg/bear_01.ogg", "U.S. Fish & Wildlife Service (fws.gov), public domain, via OpenGameArt.org (CC0)")
    # typing and a ticking clock, assembled from Kenney's clicks
    keys = [f for f in ("click1.ogg", "click2.ogg", "click3.ogg", "click4.ogg", "click5.ogg")]
    root = next(dp for dp, _, fs in os.walk(f"{DL}/kenney_ui-audio") if "click1.ogg" in fs)
    times = np.cumsum(rng.uniform(0.07, 0.19, 22))
    inputs, mix = [], []
    for i, at in enumerate(times):
        inputs += ["-i", f"{root}/{keys[i % 5]}"]
        mix.append(f"[{i}]adelay={int(at * 1000)},volume={rng.uniform(.6, 1):.2f}[a{i}]")
    graph = ";".join(mix) + ";" + "".join(f"[a{i}]" for i in range(len(times))) + f"amix=inputs={len(times)}:normalize=0[out]"
    raw = f"{TMP}/typing.wav"
    subprocess.run([FF, "-v", "error", "-y", *inputs, "-filter_complex", graph, "-map", "[out]", "-ac", "1", raw], check=True)
    copy(N, "Typing", raw, KENNEY.format(pack="UI Audio") + " (arranged)", "highpass=f=500")
    tick_root = next(dp for dp, _, fs in os.walk(f"{OLD}/kenney_interface-sounds") if "tick_001.ogg" in fs)
    inputs, mix = [], []
    for i in range(8):
        inputs += ["-i", f"{tick_root}/tick_00{1 if i % 2 == 0 else 2}.ogg"]
        mix.append(f"[{i}]adelay={i * 500}[a{i}]")
    graph = ";".join(mix) + ";" + "".join(f"[a{i}]" for i in range(8)) + "amix=inputs=8:normalize=0[out]"
    raw = f"{TMP}/clock.wav"
    subprocess.run([FF, "-v", "error", "-y", *inputs, "-filter_complex", graph, "-map", "[out]", "-ac", "1", raw], check=True)
    copy(E, "Clock Ticking", raw, KENNEY.format(pack="Interface Sounds") + " (arranged)")


def arrange(category, name, pack, files, times, af=""):
    inputs, mix = [], []
    for i, (f, at) in enumerate(zip(files, times)):
        root = next(dp for r in (f"{DL}/kenney_{pack}", f"{OLD}/kenney_{pack}") for dp, _, fs in os.walk(r) if f in fs)
        inputs += ["-i", f"{root}/{f}"]
        mix.append(f"[{i}]adelay={int(at * 1000)}[a{i}]")
    graph = ";".join(mix) + ";" + "".join(f"[a{i}]" for i in range(len(files))) + f"amix=inputs={len(files)}:normalize=0[out]"
    raw = f"{TMP}/{name}.wav"
    subprocess.run([FF, "-v", "error", "-y", *inputs, "-filter_complex", graph, "-map", "[out]", "-ac", "1", raw], check=True)
    copy(category, name, raw, KENNEY.format(pack=pack.replace("-", " ").title()) + " (arranged)", af)


def scratch_and_glitch():
    # A record scratch: a tune scrubbed back and forth like a hand on a turntable.
    src_t = t_(3)
    tune = harmonics(np.full(len(src_t), 220.0), 8) * (1 + 0.5 * np.sin(2 * np.pi * 3 * src_t)) + 0.4 * noise(3, "pink")
    d = 0.75
    t = t_(d)
    speed = np.where(t < 0.18, 1.0, np.where(t < 0.36, -2.2, np.where(t < 0.52, 2.6, -1.8)))
    speed = np.convolve(speed, np.hanning(600) / np.hanning(600).sum(), "same")
    pos = 1.0 * SR + np.cumsum(speed)
    x = np.interp(pos, np.arange(len(tune)), tune) * (np.abs(speed) > 0.05) * np.minimum(1, (d - t) / 0.05)
    write("Comedy", "Record Scratch", x, "bandpass=f=1200:width_type=o:w=3")
    # A digital glitch: stuttered, bit-crushed slices.
    parts, at = [], 0.0
    for i in range(9):
        seg = np.sign(np.sin(2 * np.pi * rng.uniform(80, 1400) * t_(rng.uniform(.02, .07)))) * rng.uniform(.4, 1)
        seg = seg + 0.5 * noise(1)[: len(seg)]
        for _ in range(int(rng.integers(1, 4))):
            parts.append((at, seg))
            at += len(seg) / SR
    write("Transitions", "Glitch", place(at + .02, parts), "acrusher=bits=6:mode=log:aa=0.5")


if __name__ == "__main__":
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(TMP, exist_ok=True)
    synth()
    scratch_and_glitch()
    packs()
    lines = ["# Sound effects library credits", "",
             "Every sound here may be used in your videos, including commercial ones, without asking: they are public domain",
             "(CC0 1.0, https://creativecommons.org/publicdomain/zero/1.0/) or were made for Mutant Video Producer and released",
             "under CC0. Credit isn't required, but the creators below appreciate it.", "",
             "Kenney's packs: https://kenney.nl/assets (Interface Sounds, UI Audio, Digital Audio, Sci-fi Sounds, Impact Sounds,",
             "RPG Audio, Casino Audio). OpenGameArt: https://opengameart.org/content/group-giggling and",
             "https://opengameart.org/content/well-done. Bear growl: U.S. Fish & Wildlife Service.", ""]
    for category in sorted(credits):
        lines += [f"## {category}", ""]
        lines += [f"- **{name}**: {source}" for name, source in sorted(credits[category])]
        lines.append("")
    open(f"{OUT}/CREDITS.md", "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    total = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(OUT) for f in fs)
    print(sum(len(v) for v in credits.values()), "sounds,", round(total / 1e6, 2), "MB")
