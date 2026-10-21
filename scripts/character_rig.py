"""Episode 3 rigs (shoulders fixed: the arms hang from inside the body, behind it): outlined cut-out characters built from curves, with a body that bobs and arms that gesture as each one
talks (their voice track's binding), eyes that blink and look at whoever is speaking, a mouth with teeth and a tongue,
and actions (enter, exit, walk, hop, jump, shake, cheer, catch, run) turned into keyframes."""
import random

W, H = 1920, 1080
KW, KH = 0.14, 0.38
AW, AH = 0.15, 0.58
RW, RH = 0.30, 0.42
BW, BH = 0.26, 0.56
OL, OW = "#1d1a1a", 0.0016
rng = random.Random(12)


# ---- helpers -----------------------------------------------------------------------------------------------------
def kf(*keys, **extra):
    return {"keyframes": [list(k) for k in keys], **extra}


def shade(hex_, f):
    """Darker (f > 0) or lighter (f < 0) version of #rrggbb."""
    r, g, b = (int(hex_[i:i + 2], 16) for i in (1, 3, 5))
    m = (lambda c: int(c * (1 - f))) if f > 0 else (lambda c: int(c + (255 - c) * -f))
    return "#%02x%02x%02x" % (m(r), m(g), m(b))


def E(lid, x, y, w, h, fill, **p):
    return {"type": "ellipse", "id": lid, "x": x, "y": y, "width": w, "height": h, "fill": fill, **p}


def R(lid, x, y, w, h, fill, **p):
    return {"type": "rect", "id": lid, "x": x, "y": y, "width": w, "height": h, "fill": fill, **p}


def P(lid, cmds, fill, x=0, y=0, w=1, h=1, **p):
    return {"type": "path", "id": lid, "x": x, "y": y, "width": w, "height": h, "commands": cmds, "fill": fill, **p}


def G(lid, parent, x=0, y=0, w=1, h=1, **p):
    g = {"type": "group", "id": lid, "x": x, "y": y, "width": w, "height": h, **p}
    if parent:
        g["parent"] = parent
    return g


def T(lid, words, x, y, w, h, size, fill="#ffffff", font="Arial Black", **p):
    return {"type": "text", "id": lid, "text": words, "x": x, "y": y, "width": w, "height": h, "font_size": size, "fill": fill,
            "font": font, "bold": True, **p}


def oval(cx, cy, rx, ry, bottom=1.0):
    """An ellipse as curves (the lower half widened by `bottom`, for a jaw)."""
    k = 0.5523
    rb = rx * bottom
    return [["M", cx + rx, cy], ["C", cx + rb, cy + k * ry, cx + k * rb, cy + ry, cx, cy + ry],
            ["C", cx - k * rb, cy + ry, cx - rb, cy + k * ry, cx - rx, cy],
            ["C", cx - rx, cy - k * ry, cx - k * rx, cy - ry, cx, cy - ry], ["C", cx + k * rx, cy - ry, cx + rx, cy - k * ry, cx + rx, cy], ["Z"]]


def fuzz(cx, cy, rx, ry, n=11):
    """A pompom: a circle of bumps."""
    import math
    cmds = []
    for i in range(n + 1):
        a = 2 * math.pi * i / n
        x, y = cx + rx * math.cos(a), cy + ry * math.sin(a)
        if i == 0:
            cmds.append(["M", x, y])
        else:
            am = 2 * math.pi * (i - 0.5) / n
            cmds.append(["Q", cx + rx * 1.35 * math.cos(am), cy + ry * 1.35 * math.sin(am), x, y])
    return cmds + [["Z"]]


def talk(cid, base, amount, **extra):
    return {"audio": cid.lower(), "base": base, "amount": amount, "channel": "peak", "attack": 0.02, "release": 0.1, **extra}


def blink():
    b = rng.uniform(2.0, 4.5)
    return kf([0, 1], [b, 1], [b + 0.05, 0.08], [b + 0.12, 1], loop=round(b + rng.uniform(0.8, 2.2), 2))


def stroke(**p):
    return {"stroke": OL, "stroke_width": OW, **p}


def clean(keys):
    """Keyframes sorted, with strictly rising times."""
    keys = sorted(keys, key=lambda k: k[0])
    out = []
    for k in keys:
        k = list(k)
        if out and k[0] <= out[-1][0]:
            k[0] = round(out[-1][0] + 0.01, 3)
        out.append(k)
    return out


# ---- motion: actions -> keyframes --------------------------------------------------------------------------------
class Motion:
    """Keyframes for a character's root (x, y offset, rotation) and arms (extra rotation), from the shot's actions."""

    def __init__(self, home_x, width, acts, length, running=False):
        self.x = [[0, home_x]]
        self.y = [[0, 0]]
        self.rot = [[0, 0]]
        self.arm = [[0, 0]]
        self.width, self.length, self.running = width, length, running
        self.head = [[0, 0]]
        self.squash = []
        cur = home_x
        enters = [a for a in acts if a[1] == "enter"]
        if enters:
            side = enters[0][2]
            self.x = [[0, -width - 0.06 if side == "left" else 1.06]]
            cur = self.x[0][1]
        for t, kind, *args in acts:
            if kind == "enter":
                dur = float(args[1]) if len(args) > 1 else 1.2
                self.x += [[t, cur], [t + dur, home_x, "ease_out"]]
                self._bob(t, dur)
                cur = home_x
            elif kind == "exit":
                dur = float(args[1]) if len(args) > 1 else 1.2
                to = 1.08 if args[0] == "right" else -width - 0.08
                self.x += [[t, cur, "ease_in"], [t + dur, to]]
                self._bob(t, dur)
                cur = to
            elif kind == "walk":
                to, dur = float(args[0]), float(args[1])
                self.x += [[t, cur, "ease_in_out"], [t + dur, to]]
                self._bob(t, dur)
                cur = to
            elif kind == "hop":
                self.y += [[t, 0, "ease_out"], [t + 0.15, -0.035, "ease_in"], [t + 0.32, 0]]
            elif kind == "jump":
                self.y += [[t, 0, "ease_out"], [t + 0.25, -0.1, "ease_in"], [t + 0.55, 0, "ease_out"], [t + 0.65, -0.01, "ease_in"], [t + 0.75, 0]]
                self.arm += [[t, 0], [t + 0.2, 1], [t + 0.75, 1], [t + 0.95, 0]]
            elif kind == "shake":
                dur = float(args[0]) if args else 1.0
                n = int(dur / 0.06)
                self.rot += [[t + i * 0.06, (4 if i % 2 else -4) if i < n else 0] for i in range(n + 1)]
            elif kind == "cheer":
                self.arm += [[t, 0, "ease_out"], [t + 0.25, 1], [t + 1.6, 1, "ease_in"], [t + 1.9, 0]]
                self.y += [[t, 0, "ease_out"], [t + 0.15, -0.03, "ease_in"], [t + 0.3, 0, "ease_out"], [t + 0.45, -0.03, "ease_in"], [t + 0.6, 0]]
            elif kind == "punches":
                dur = float(args[0]) if args else 4.0
                n = int(dur / 0.4)
                for i in range(n):
                    a_ = t + i * 0.4
                    self.arm += [[a_, 0, "ease_out"], [a_ + 0.12, 0.42 if i % 2 else 0.3], [a_ + 0.3, 0]]
                    self.rot += [[a_, 0, "ease_out"], [a_ + 0.12, 3 if i % 2 else -3], [a_ + 0.3, 0]]
            elif kind == "sniff":
                self.head += [[t, 0, "ease_in_out"], [t + 0.3, -6], [t + 0.6, 4], [t + 0.9, -6], [t + 1.2, 0]]
                self.y += [[t, 0, "ease_in_out"], [t + 0.3, 0.01], [t + 0.6, 0], [t + 0.9, 0.01], [t + 1.2, 0]]
            elif kind == "sit":
                self.y += [[t, 0, "ease_in_out"], [t + 0.5, 0.07]]
                self.sat = t
            elif kind == "headshake":
                self.head += [[t + i * 0.18, (12 if i % 2 else -12) if i < 6 else 0, "ease_in_out"] for i in range(7)]
            elif kind == "hug":
                self.arm += [[t, 0, "ease_out"], [t + 0.4, -0.55], [t + 1.8, -0.55, "ease_in"], [t + 2.2, 0]]
            elif kind == "swing":
                self.same = True
                self.arm += [[t, 0, "ease_in_out"], [t + 0.7, -0.45, "ease_in"], [t + 0.95, 0.9, "ease_out"], [t + 1.8, 0.9, "ease_in_out"], [t + 2.3, 0]]
            elif kind == "catch":
                dur = float(args[0]) if args else 4.0
                self.arm += [[t, 0, "ease_out"], [t + 0.3, 1], [t + dur - 0.3, 1, "ease_in"], [t + dur, 0]]
                steps = int(dur / 0.7)
                for i in range(steps):
                    a = t + i * 0.7
                    self.y += [[a, 0, "ease_out"], [a + 0.18, -0.03, "ease_in"], [a + 0.36, 0]]
                    self.x += [[a, cur + (0.02 if i % 2 else -0.02), "ease_in_out"]]
                self.x += [[t + dur, cur]]

    def _bob(self, t, dur):
        steps = max(1, int(dur / 0.22))
        for i in range(steps):
            a = t + i * dur / steps
            self.y += [[a, 0, "ease_out"], [a + dur / steps / 2, -0.018, "ease_in"]]
        self.y += [[t + dur, 0]]

    def root(self, home_y):
        if self.running and not getattr(self, "fly", False):
            return {"x": self._prop(self.x), "y": kf([0, home_y, "ease_in_out"], [0.16, home_y - 0.02], loop=0.32, ping_pong=True),
                    "rotation": 3}
        y = clean(self.y)
        if getattr(self, "sat", None) is not None:  # sitting stays sat
            y = [k if k[0] < self.sat + 0.5 else [k[0], max(k[1], 0.07)] + k[2:] for k in y]
        out = {"x": self._prop(self.x), "y": self._prop([[k[0], home_y + k[1]] + k[2:] for k in y]), "rotation": self._prop(self.rot)}
        if self.squash:
            out["scale_y"] = {"keyframes": clean([[0, 1]] + self.squash)}
        else:
            period = round(rng.uniform(1.5, 2.1), 2)
            out["scale_y"] = kf([0, 1.0, "ease_in_out"], [period, 1.012], loop=period, ping_pong=True)
        return out

    def _prop(self, keys):
        keys = clean(keys)
        return keys[0][1] if len(keys) == 1 else {"keyframes": keys}

    def arms(self, up_l, up_r):
        """Extra arm rotation (outer arm group) for cheer/catch/jump, or a swing when running."""
        if self.running:
            return (kf([0, -35, "ease_in_out"], [0.16, 35], loop=0.32, ping_pong=True), kf([0, 35, "ease_in_out"], [0.16, -35], loop=0.32, ping_pong=True))
        a = clean(self.arm)
        if len(a) == 1:
            return 0, 0
        if getattr(self, "same", False):  # a two-handed swing: both arms the same way
            up_r = -up_r
        return {"keyframes": [[k[0], k[1] * up_l] + k[2:] for k in a]}, {"keyframes": [[k[0], k[1] * up_r] + k[2:] for k in a]}


def look_keys(cid, cast_x, lines, width_of):
    """Pupil offsets (fraction of the eye group's width): everyone looks at whoever is talking."""
    me = cast_x[cid] + width_of(cid) / 2
    keys, prev = [[0, 0]], 0.0
    for who, s, e, *_ in lines:
        if who not in cast_x:
            continue
        if who == cid:
            v = 0.0
        else:
            other = cast_x[who] + width_of(who) / 2
            v = max(-0.09, min(0.09, (other - me) * 0.35))
        if abs(v - prev) > 1e-3:
            keys += [[round(s, 3), prev, "ease_out"], [round(s + 0.18, 3), v]]
            prev = v
    return {"keyframes": clean(keys)} if len(keys) > 1 else 0


# ---- the parts every character shares ----------------------------------------------------------------------------
def eyes(cid, parent, x, y, w, h, look, pupil=OL, white="#ffffff", lids=None, lashes=False):
    g, lk = f"{cid}_eyes", f"{cid}_look"
    L = [G(g, parent, x, y, w, h, anchor_y=0.5, scale_y=blink()),
         E(f"{g}_l", 0, 0, 0.53, 1, white, rotation=-5, parent=g, **stroke()),
         E(f"{g}_r", 0.47, 0, 0.53, 1, white, rotation=5, parent=g, **stroke()),
         G(lk, g, look, 0, 1, 1),
         E(f"{lk}_l", 0.30, 0.38, 0.11, 0.30, pupil, parent=lk), E(f"{lk}_r", 0.59, 0.38, 0.11, 0.30, pupil, parent=lk),
         E(f"{lk}_gl", 0.33, 0.42, 0.035, 0.09, "#ffffff", parent=lk), E(f"{lk}_gr", 0.62, 0.42, 0.035, 0.09, "#ffffff", parent=lk)]
    if lids:  # the top of each eye, its lower edge curved: inside the eye's own outline
        lid = [["M", 0, 0.5], ["C", 0, 0.16, 0.22, 0, 0.5, 0], ["C", 0.78, 0, 1, 0.16, 1, 0.5], ["Q", 0.5, 0.34, 0, 0.5], ["Z"]]
        L += [P(f"{g}_lidl", lid, lids, 0, 0, 0.53, 1, rotation=-5, parent=g, **stroke()),
              P(f"{g}_lidr", lid, lids, 0.47, 0, 0.53, 1, rotation=5, parent=g, **stroke())]
    if lashes:
        L += [P(f"{g}_lash", [["M", 0.02, 0.25], ["L", -0.06, 0.12], ["M", 0.08, 0.12], ["L", 0.02, -0.02], ["M", 0.98, 0.25], ["L", 1.06, 0.12],
                               ["M", 0.92, 0.12], ["L", 0.98, -0.02]], "#00000000", parent=g, stroke=OL, stroke_width=0.0022)]
    return L


def viseme(cid, shape):
    """1 while the character's voice is making this mouth shape (A, E, O, L, F, M, rest), else 0."""
    return {"audio": cid.lower(), "viseme": shape, "base": 0, "amount": 1}


def mouth(cid, parent, x, y, w, h, lips=None, big=1.0):
    """A mouth that opens with the voice's loudness and takes the shape of the words: open shapes (A, E, O, L, F) in a
    group that scales with the sound, closed ones (rest, M) beside it."""
    m = f"{cid}_mouth"
    edge = {"stroke": lips or OL, "stroke_width": OW * (2.2 if lips else 1)}
    L = [G(m, parent, x, y, w, h, anchor_y=0.0, scale_y=talk(cid, 0.1, 2.5 * big), scale_x=talk(cid, 0.85, 0.4))]

    def opened(s, sx=1.0, sy=1.0, teeth=0.3, tongue=True, lip=False):
        g = f"{m}_{s}"
        out = [G(g, m, (1 - sx) / 2, 0, sx, sy, opacity=viseme(cid, s)),
               E(f"{g}_bg", 0, 0, 1, 1, "#3b0d0d", parent=g, **edge)]
        if teeth:
            out.append(R(f"{g}_teeth", 0.08, 0, 0.84, teeth, "#ffffff", parent=g,
                         mask={"type": "ellipse", "x": -0.095, "y": 0, "width": 1.19, "height": 1 / teeth}))
        if tongue:
            out.append(E(f"{g}_tongue", 0.2, 0.55, 0.6, 0.6, "#e56b6f", parent=g,
                         mask={"type": "ellipse", "x": -0.333, "y": -0.917, "width": 1.667, "height": 1.667}))
        if lip:  # (the lower lip tucked under the top teeth)
            out.append(R(f"{g}_lip", 0.1, 0.5, 0.8, 0.5, lips or "#d98c8c", parent=g, radius=0.5))
        return out

    L += opened("A") + opened("L", 0.75, 0.6, tongue=False) + opened("E", 1.15, 0.6, teeth=0.5, tongue=False)
    L += opened("O", 0.55, 1.1, teeth=0) + opened("F", 0.9, 0.7, teeth=0.45, tongue=False, lip=True)
    shut = f"{m}_shut"
    L += [G(shut, parent, x, y, w, h * 0.3),
          P(f"{shut}_rest", [["M", 0.05, 0.3], ["Q", 0.5, 1.0, 0.95, 0.3]], "#00000000", parent=shut, stroke=lips or OL,
            stroke_width=OW * 2.2, opacity=viseme(cid, "rest")),
          P(f"{shut}_m", [["M", 0.1, 0.5], ["Q", 0.5, 0.75, 0.9, 0.5]], "#00000000", parent=shut, stroke=lips or OL,
            stroke_width=OW * 3, opacity=viseme(cid, "M"))]
    return L


def arm(cid, side, parent, x, y, w, h, sleeve, hand, extra, gesture, mitten=True):
    """Outer group (actions) > inner group (talking) > sleeve and hand, pivoting at the shoulder."""
    o, i = f"{cid}_arm{side}", f"{cid}_arm{side}_t"
    return [G(o, parent, x, y, w, h, anchor_x=0.5, anchor_y=0.08, rotation=extra),
            G(i, o, 0, 0, 1, 1, anchor_x=0.5, anchor_y=0.08, rotation=gesture),
            P(f"{i}_sleeve", [["M", 0.28, 0.1], ["Q", 0.28, -0.02, 0.5, -0.02], ["Q", 0.72, -0.02, 0.72, 0.1], ["L", 0.79, 0.74], ["Q", 0.5, 0.84, 0.21, 0.74],
                              ["Z"]], sleeve, parent=i, **stroke()),
            E(f"{i}_hand", 0.14, 0.66, 0.72, 0.34 if mitten else 0.26, hand, parent=i, **stroke())]


# ---- kids --------------------------------------------------------------------------------------------------------
def kid(cid, c, x, y_home, motion, look, front=None):
    a = KW * W / (KH * H)
    b = f"{cid}_b"
    root = {**G(cid, None, 0, 0, KW, KH, anchor_y=1.0), **motion.root(y_home)}
    L = [root, E(f"{cid}_shadow", 0.06, 0.95, 0.88, 0.07, "#000000", opacity=0.16, parent=cid),
         G(b, cid, 0, talk(cid, 0, -0.045), 1, 1, anchor_y=1.0, rotation=talk(cid, 0, 2.5))]
    add = lambda layer: L.append({**layer, "parent": layer.get("parent", b)})  # noqa
    fat = c.get("chubby")
    up_l, up_r = 150, -150
    ex_l, ex_r = motion.arms(up_l, up_r)
    run = motion.running
    add(E(f"{cid}_f1", 0.17, kf([0, 0.895, "ease_in_out"], [0.16, 0.86], loop=0.32, ping_pong=True) if run else 0.895, 0.30, 0.095, "#2b2b2b",
          **stroke()))
    add(E(f"{cid}_f2", 0.53, kf([0, 0.86, "ease_in_out"], [0.16, 0.895], loop=0.32, ping_pong=True) if run else 0.895, 0.30, 0.095, "#2b2b2b",
          **stroke()))
    if c.get("hair"):
        add(P(f"{cid}_hairback", [["M", 0.09, 0.30], ["C", 0.0, 0.5, 0.03, 0.70, 0.12, 0.74], ["L", 0.88, 0.74], ["C", 0.97, 0.70, 1.0, 0.5, 0.91, 0.30], ["Z"]],
              c["hair"], **stroke()))
    s0, s1 = (0.08, 0.92) if fat else (0.2, 0.8)
    L += arm(cid, "L", b, s0 + 0.035 - 0.085, 0.621, 0.17, 0.29, c["jacket"], c["mitten"], ex_l, talk(cid, 12, 26))
    L += arm(cid, "R", b, s1 - 0.035 - 0.085, 0.621, 0.17, 0.29, c["jacket"], c["mitten"], ex_r, talk(cid, -12, -26))
    add(P(f"{cid}_jacket", [["M", 0.5, 0.575], ["C", s0 + 0.25, 0.575, s0 + 0.05, 0.6, s0, 0.69], ["C", s0 - 0.04, 0.79, s0 - 0.05, 0.88, s0 - 0.03, 0.945],
                            ["Q", 0.5, 0.985, s1 + 0.03, 0.945], ["C", s1 + 0.05, 0.88, s1 + 0.04, 0.79, s1, 0.69], ["C", s1 - 0.05, 0.6, s1 - 0.25, 0.575, 0.5, 0.575],
                            ["Z"]], c["jacket"], **stroke()))
    add(P(f"{cid}_jshade", [["M", s1 - 0.05, 0.62], ["C", s1 + 0.03, 0.7, s1 + 0.04, 0.84, s1 + 0.01, 0.94], ["Q", 0.66, 0.97, 0.6, 0.965],
                            ["C", 0.7, 0.85, 0.7, 0.7, s1 - 0.05, 0.62], ["Z"]], shade(c["jacket"], 0.18), opacity=0.7))
    add(P(f"{cid}_zip", [["M", 0.5, 0.61], ["L", 0.5, 0.95]], "#00000000", stroke=shade(c["jacket"], 0.4), stroke_width=0.0018))
    add(P(f"{cid}_collar", [["M", 0.33, 0.585], ["Q", 0.5, 0.64, 0.67, 0.585], ["L", 0.65, 0.625], ["Q", 0.5, 0.685, 0.35, 0.625], ["Z"]],
          shade(c["jacket"], 0.25), **stroke()))
    add(P(f"{cid}_pocket", [["M", 0.3, 0.8], ["Q", 0.36, 0.83, 0.42, 0.8], ["M", 0.58, 0.8], ["Q", 0.64, 0.83, 0.7, 0.8]], "#00000000",
          stroke=shade(c["jacket"], 0.4), stroke_width=0.0015))
    if front:
        L += front
    if c.get("hood"):
        add(P(f"{cid}_hood", oval(0.5, 0.37, 0.30 / a, 0.31, 1.03), c["hat"], **stroke()))
    add(P(f"{cid}_head", oval(0.5, 0.37, 0.265 / a, 0.265, 1.05), c["skin"], **stroke()))
    add(P(f"{cid}_cheek", [["M", 0.55, 0.11], ["C", 0.8, 0.12, 0.93, 0.27, 0.91, 0.42], ["C", 0.89, 0.56, 0.76, 0.63, 0.55, 0.635],
                           ["C", 0.72, 0.6, 0.83, 0.5, 0.84, 0.38], ["C", 0.85, 0.25, 0.75, 0.14, 0.55, 0.11], ["Z"]], shade(c["skin"], 0.14), opacity=0.55))
    if cid == "Marnie":
        add(E(f"{cid}_blush1", 0.2, 0.47, 0.12, 0.05, "#e5989b", opacity=0.6))
        add(E(f"{cid}_blush2", 0.68, 0.47, 0.12, 0.05, "#e5989b", opacity=0.6))
    if c.get("hood"):
        add(P(f"{cid}_rim", [["M", 0.1, 0.38], ["C", 0.1, 0.13, 0.3, 0.06, 0.5, 0.06], ["C", 0.7, 0.06, 0.9, 0.13, 0.9, 0.38], ["C", 0.8, 0.2, 0.66, 0.15, 0.5, 0.15],
                              ["C", 0.34, 0.15, 0.2, 0.2, 0.1, 0.38], ["Z"]], c["hat"], **stroke()))
        add(P(f"{cid}_strings", [["M", 0.3, 0.6], ["L", 0.28, 0.72], ["M", 0.7, 0.6], ["L", 0.72, 0.72]], "#00000000", stroke="#1d3557",
              stroke_width=0.002))
    else:
        if c.get("flaps"):
            add(P(f"{cid}_flapL", [["M", 0.08, 0.27], ["Q", 0.03, 0.42, 0.12, 0.5], ["Q", 0.19, 0.42, 0.18, 0.29], ["Z"]], c["hat"], **stroke()))
            add(P(f"{cid}_flapR", [["M", 0.92, 0.27], ["Q", 0.97, 0.42, 0.88, 0.5], ["Q", 0.81, 0.42, 0.82, 0.29], ["Z"]], c["hat"], **stroke()))
        if c.get("fringe"):
            add(P(f"{cid}_fringe", [["M", 0.24, 0.29], ["L", 0.3, 0.355], ["L", 0.36, 0.3], ["L", 0.43, 0.36], ["L", 0.5, 0.3], ["L", 0.57, 0.36], ["L", 0.64, 0.3],
                                    ["L", 0.7, 0.355], ["L", 0.76, 0.29], ["Z"]], c["fringe"], **stroke()))
        if c.get("hair"):
            add(P(f"{cid}_bangs", [["M", 0.18, 0.29], ["Q", 0.34, 0.38, 0.5, 0.31], ["Q", 0.66, 0.38, 0.82, 0.29], ["Z"]], c["hair"], **stroke()))
        add(P(f"{cid}_hat", [["M", 0.1, 0.27], ["C", 0.1, 0.06, 0.3, -0.01, 0.5, -0.01], ["C", 0.7, -0.01, 0.9, 0.06, 0.9, 0.27], ["Z"]], c["hat"], **stroke()))
        add(P(f"{cid}_hatshine", [["M", 0.24, 0.13], ["Q", 0.36, 0.03, 0.54, 0.035], ["Q", 0.38, 0.07, 0.29, 0.16], ["Z"]], "#ffffff", opacity=0.3))
        add(P(f"{cid}_band", [["M", 0.07, 0.235], ["Q", 0.5, 0.18, 0.93, 0.235], ["L", 0.935, 0.315], ["Q", 0.5, 0.26, 0.065, 0.315], ["Z"]], c["band"],
              **stroke()))
        ribs = []
        for i in range(1, 12):
            xx = 0.07 + i * 0.86 / 12
            yy = 0.235 - 0.055 * (1 - ((xx - 0.5) / 0.43) ** 2)
            ribs += [["M", xx, yy + 0.008], ["L", xx, yy + 0.07]]
        add(P(f"{cid}_ribs", ribs, "#00000000", stroke=shade(c["band"], 0.25), stroke_width=0.0012))
        if c.get("pom"):
            add(P(f"{cid}_pom", fuzz(0.5, 0.0, 0.065 / a, 0.065), c["pom"], **stroke()))
    if c.get("tophat"):
        add(P(f"{cid}_tophat", [["M", 0.2, 0.2], ["L", 0.8, 0.2], ["L", 0.76, -0.22], ["Q", 0.5, -0.25, 0.24, -0.22], ["Z"]], "#111111", **stroke()))
        add(P(f"{cid}_tophatband", [["M", 0.215, 0.1], ["L", 0.785, 0.1], ["L", 0.78, 0.17], ["L", 0.22, 0.17], ["Z"]], "#c1121f"))
        add(P(f"{cid}_tophatbrim", [["M", 0.08, 0.2], ["Q", 0.5, 0.15, 0.92, 0.2], ["Q", 0.94, 0.25, 0.88, 0.24], ["Q", 0.5, 0.21, 0.12, 0.24],
                                    ["Q", 0.06, 0.25, 0.08, 0.2], ["Z"]], "#111111", **stroke()))
    L += eyes(cid, b, 0.225, 0.255, 0.55, 0.19, look, lids=c["skin"] if c.get("lids") else None, lashes=c.get("lashes"))
    L += mouth(cid, b, 0.405, 0.5, 0.19, 0.09)
    return L


# ---- adults ------------------------------------------------------------------------------------------------------
def adult(cid, c, x, y_home, motion, look):
    a = AW * W / (AH * H)
    b = f"{cid}_b"
    L = [{**G(cid, None, 0, 0, AW, AH, anchor_y=1.0), **motion.root(y_home)},
         E(f"{cid}_shadow", 0.04, 0.965, 0.92, 0.05, "#000000", opacity=0.16, parent=cid),
         G(b, cid, 0, talk(cid, 0, -0.025), 1, 1, anchor_y=1.0, rotation=talk(cid, 0, 1.5))]
    add = lambda layer: L.append({**layer, "parent": layer.get("parent", b)})  # noqa
    ex_l, ex_r = motion.arms(140, -140)
    run = motion.running
    add(R(f"{cid}_legL", 0.3, 0.78, 0.17, 0.17, c["pants"], **stroke()))
    add(R(f"{cid}_legR", 0.53, 0.78, 0.17, 0.17, c["pants"], **stroke()))
    add(E(f"{cid}_shoeL", 0.22, kf([0, 0.93, "ease_in_out"], [0.16, 0.9], loop=0.32, ping_pong=True) if run else 0.93, 0.28, 0.065, "#1a1a1a"))
    add(E(f"{cid}_shoeR", 0.5, kf([0, 0.9, "ease_in_out"], [0.16, 0.93], loop=0.32, ping_pong=True) if run else 0.93, 0.28, 0.065, "#1a1a1a"))
    if c["style"] == "bouffant":
        add(P(f"{cid}_hairback", fuzz(0.5, 0.17, 0.4, 0.18, 9), c["hair"], **stroke()))
    if c["style"] == "curls":
        add(P(f"{cid}_hairback", fuzz(0.5, 0.18, 0.37, 0.17, 14), c["hair"], **stroke()))
    L += arm(cid, "L", b, 0.235 - 0.1, 0.4 - 0.034, 0.2, 0.42, c["suit"], c["skin"], ex_l, talk(cid, 10, 24), mitten=False)
    L += arm(cid, "R", b, 0.765 - 0.1, 0.4 - 0.034, 0.2, 0.42, c["suit"], c["skin"], ex_r, talk(cid, -10, -24), mitten=False)
    add(P(f"{cid}_torso", [["M", 0.5, 0.355], ["C", 0.3, 0.355, 0.2, 0.37, 0.18, 0.45], ["C", 0.14, 0.55, 0.14, 0.7, 0.17, 0.82], ["L", 0.83, 0.82],
                           ["C", 0.86, 0.7, 0.86, 0.55, 0.82, 0.45], ["C", 0.8, 0.37, 0.7, 0.355, 0.5, 0.355], ["Z"]], c["suit"], **stroke()))
    add(P(f"{cid}_tshade", [["M", 0.66, 0.39], ["C", 0.84, 0.45, 0.88, 0.65, 0.83, 0.82], ["L", 0.7, 0.82], ["C", 0.76, 0.65, 0.74, 0.48, 0.66, 0.39], ["Z"]],
          shade(c["suit"], 0.2), opacity=0.7))
    add(P(f"{cid}_shirt", [["M", 0.38, 0.37], ["L", 0.62, 0.37], ["L", 0.5, 0.56], ["Z"]], c["shirt"], **stroke()))
    add(P(f"{cid}_lapels", [["M", 0.38, 0.37], ["L", 0.5, 0.56], ["L", 0.42, 0.52], ["L", 0.31, 0.42], ["Z"], ["M", 0.62, 0.37], ["L", 0.5, 0.56], ["L", 0.58, 0.52],
                            ["L", 0.69, 0.42], ["Z"]], shade(c["suit"], 0.15), **stroke()))
    if c.get("tie"):
        add(P(f"{cid}_tie", [["M", 0.47, 0.375], ["L", 0.53, 0.375], ["L", 0.55, 0.44], ["L", 0.5, 0.56], ["L", 0.45, 0.44], ["Z"]], c["tie"], **stroke()))
    add(E(f"{cid}_btn1", 0.49, 0.62, 0.02, 0.012, shade(c["suit"], 0.4)))
    add(E(f"{cid}_btn2", 0.49, 0.7, 0.02, 0.012, shade(c["suit"], 0.4)))
    add(R(f"{cid}_neck", 0.42, 0.31, 0.16, 0.07, c["skin"], **stroke()))
    add(E(f"{cid}_earL", 0.17, 0.18, 0.08, 0.06, c["skin"], **stroke()))
    add(E(f"{cid}_earR", 0.75, 0.18, 0.08, 0.06, c["skin"], **stroke()))
    add(P(f"{cid}_head", oval(0.5, 0.2, 0.16 / a, 0.16, 1.06), c["skin"], **stroke()))
    add(P(f"{cid}_cheek", [["M", 0.55, 0.045], ["C", 0.78, 0.05, 0.87, 0.14, 0.86, 0.22], ["C", 0.85, 0.3, 0.74, 0.36, 0.55, 0.36],
                           ["C", 0.7, 0.33, 0.78, 0.28, 0.79, 0.21], ["C", 0.8, 0.13, 0.72, 0.07, 0.55, 0.045], ["Z"]], shade(c["skin"], 0.14), opacity=0.55))
    st = c["style"]
    if st == "bald":
        add(P(f"{cid}_tufts", [["M", 0.16, 0.2], ["Q", 0.14, 0.1, 0.24, 0.09], ["Q", 0.21, 0.15, 0.22, 0.22], ["Z"], ["M", 0.84, 0.2], ["Q", 0.86, 0.1, 0.76, 0.09],
                               ["Q", 0.79, 0.15, 0.78, 0.22], ["Z"]], c["hair"], **stroke()))
        add(P(f"{cid}_comb", [["M", 0.3, 0.07], ["Q", 0.5, 0.025, 0.72, 0.07], ["M", 0.3, 0.085], ["Q", 0.5, 0.045, 0.7, 0.09]], "#00000000", stroke=c["hair"],
              stroke_width=0.002))
    elif st == "slick":
        add(P(f"{cid}_hair", [["M", 0.17, 0.2], ["C", 0.14, 0.04, 0.4, 0.0, 0.55, 0.02], ["C", 0.75, 0.03, 0.87, 0.08, 0.83, 0.2], ["Q", 0.78, 0.1, 0.62, 0.09],
                              ["L", 0.58, 0.06], ["Q", 0.4, 0.09, 0.2, 0.12], ["Z"]], c["hair"], **stroke()))
    elif st == "cap":
        add(P(f"{cid}_cap", [["M", 0.15, 0.13], ["C", 0.15, 0.0, 0.35, -0.03, 0.5, -0.03], ["C", 0.65, -0.03, 0.85, 0.0, 0.85, 0.13], ["Z"]], c["cap"], **stroke()))
        add(P(f"{cid}_brim", [["M", 0.12, 0.13], ["L", 0.88, 0.13], ["Q", 1.0, 0.14, 0.98, 0.17], ["Q", 0.5, 0.15, 0.12, 0.155], ["Z"]], shade(c["cap"], 0.25),
              **stroke()))
        add(T(f"{cid}_capt", "BAIT", 0.3, 0.035, 0.4, 0.06, 0.014, fill="#ffffff", align="center"))
    elif st in ("bouffant", "curls"):
        add(P(f"{cid}_bangs", fuzz(0.5, 0.06, 0.3, 0.06, 8), c["hair"], **stroke()))
    if c.get("beard"):
        add(P(f"{cid}_beard", [["M", 0.17, 0.2], ["C", 0.18, 0.33, 0.33, 0.39, 0.5, 0.39], ["C", 0.67, 0.39, 0.82, 0.33, 0.83, 0.2], ["Q", 0.8, 0.29, 0.65, 0.27],
                               ["Q", 0.5, 0.22, 0.35, 0.27], ["Q", 0.2, 0.29, 0.17, 0.2], ["Z"]], c["hair"], **stroke()))
    L += eyes(cid, b, 0.31, 0.135, 0.38, 0.075, look)
    if c.get("glasses"):
        add(P(f"{cid}_glasses", oval(0.405, 0.172, 0.095, 0.045) + oval(0.595, 0.172, 0.095, 0.045) + [["M", 0.5, 0.17], ["L", 0.5, 0.17]], "#00000000",
              stroke=OL, stroke_width=0.0028))
    add(P(f"{cid}_brows", [["M", 0.32, 0.125], ["Q", 0.4, 0.105, 0.47, 0.122], ["M", 0.53, 0.122], ["Q", 0.6, 0.105, 0.68, 0.125]], "#00000000",
          stroke=c["hair"] if c["style"] != "bald" else OL, stroke_width=0.0035, y=talk(cid, 0, -0.012)))
    L += mouth(cid, b, 0.42, 0.25, 0.16, 0.045, lips="#a4133c" if c.get("lipstick") else None)
    if c.get("pie"):
        add(P(f"{cid}_pie", fuzz(0.5, 0.265, 0.17, 0.05, 9), "#9d0208", opacity=0.92))
        add(E(f"{cid}_pie2", 0.62, 0.2, 0.08, 0.03, "#9d0208", opacity=0.9))
        add(E(f"{cid}_crumb1", 0.3, 0.29, 0.03, 0.012, "#e9c46a"))
        add(E(f"{cid}_crumb2", 0.66, 0.3, 0.025, 0.01, "#e9c46a"))
    return L


# ---- Plowbot -----------------------------------------------------------------------------------------------------
def robot(x, y_home, motion, look, sash=False, moving=None):
    cid, b = "Plowbot", "Plowbot_b"
    a = RW * W / (RH * H)
    root = motion.root(y_home)
    L = [{**G(cid, None, 0, 0, RW, RH, anchor_y=1.0, scale_x=-1), **root},
         E("pb_shadow", 0.02, 0.95, 0.92, 0.06, "#000000", opacity=0.18, parent=cid),
         G(b, cid, 0, talk(cid, 0, -0.02), 1, 1, anchor_y=1.0)]
    add = lambda layer: L.append({**layer, "parent": layer.get("parent", b)})  # noqa
    add(P("pb_pipe", [["M", 0.3, 0.42], ["L", 0.3, 0.2], ["L", 0.36, 0.2], ["L", 0.36, 0.42], ["Z"]], "#6c757d", **stroke()))
    add({"type": "particles", "id": "pb_smoke", "x": 0.26, "y": 0.0, "width": 0.14, "height": 0.2, "count": 60, "lifetime": 1.6, "speed": 0.5,
         "direction": -90, "spread": 25, "gravity": -0.1, "size": 0.012, "colors": ["#ced4da", "#adb5bd"], "seed": 4})
    add(P("pb_tread", [["M", 0.12, 0.8], ["L", 0.72, 0.8], ["Q", 0.8, 0.8, 0.8, 0.89], ["Q", 0.8, 0.98, 0.72, 0.98], ["L", 0.12, 0.98], ["Q", 0.04, 0.98, 0.04, 0.89],
                      ["Q", 0.04, 0.8, 0.12, 0.8], ["Z"]], "#2b2d42", **stroke()))
    add(P("pb_treadlink", [["M", 0.12, 0.815], ["L", 0.72, 0.815], ["Q", 0.785, 0.815, 0.785, 0.89], ["Q", 0.785, 0.965, 0.72, 0.965], ["L", 0.12, 0.965],
                           ["Q", 0.055, 0.965, 0.055, 0.89], ["Q", 0.055, 0.815, 0.12, 0.815], ["Z"]], "#00000000", stroke="#8d99ae", stroke_width=0.003,
          stroke_dash=[1.5, 1.5], dash_offset=moving if moving is not None else 0))
    for i, wx in enumerate((0.16, 0.32, 0.48, 0.64)):
        add(P(f"pb_wheel{i}", oval(wx, 0.89, 0.055 / a, 0.055), "#8d99ae", **stroke()))
        add(P(f"pb_hub{i}", oval(wx, 0.89, 0.02 / a, 0.02), "#495057"))
    add(P("pb_body", [["M", 0.14, 0.42], ["L", 0.7, 0.42], ["Q", 0.75, 0.42, 0.75, 0.47], ["L", 0.75, 0.8], ["L", 0.09, 0.8], ["L", 0.09, 0.47],
                      ["Q", 0.09, 0.42, 0.14, 0.42], ["Z"]], "#ffb703", **stroke()))
    add(P("pb_bodyshade", [["M", 0.09, 0.7], ["L", 0.75, 0.7], ["L", 0.75, 0.8], ["L", 0.09, 0.8], ["Z"]], "#e09f00"))
    add(R("pb_stripe", 0.09, 0.6, 0.66, 0.05, "#023047"))
    add(T("pb_name", "PLOWBOT 3000", 0.11, 0.705, 0.62, 0.08, 0.016, fill="#023047", align="center"))
    for i, (rx, ry) in enumerate(((0.12, 0.45), (0.72, 0.45), (0.12, 0.77), (0.72, 0.77))):
        add(P(f"pb_rivet{i}", oval(rx, ry, 0.008 / a, 0.008), "#c77d00"))
    add(G("pb_claw", b, 0.36, 0.3, 0.1, 0.24, anchor_x=0.5, anchor_y=0.1, rotation=talk(cid, 15, -45)))
    add(R("pb_clawarm", 0.35, 0.0, 0.3, 0.8, "#6c757d", parent="pb_claw", **stroke()))
    add(P("pb_clawhand", [["M", 0.1, 0.75], ["L", 0.9, 0.75], ["L", 0.75, 1.0], ["L", 0.5, 0.85], ["L", 0.25, 1.0], ["Z"]], "#495057", parent="pb_claw", **stroke()))
    add(P("pb_cab", [["M", 0.46, 0.12], ["L", 0.7, 0.12], ["Q", 0.75, 0.12, 0.75, 0.17], ["L", 0.75, 0.46], ["L", 0.41, 0.46], ["L", 0.41, 0.17],
                     ["Q", 0.41, 0.12, 0.46, 0.12], ["Z"]], "#ffb703", **stroke()))
    add(P("pb_win", [["M", 0.47, 0.16], ["L", 0.69, 0.16], ["Q", 0.715, 0.16, 0.715, 0.19], ["L", 0.715, 0.38], ["L", 0.445, 0.38], ["L", 0.445, 0.19],
                     ["Q", 0.445, 0.16, 0.47, 0.16], ["Z"]], "#0b2545", **stroke()))
    add(E("pb_beacon", 0.555, 0.03, 0.05, 0.1, "#fb5607", opacity=kf([0, 1], [0.4, 0.35], [0.8, 1], loop=0.8),
          glow={"color": "#fb5607", "opacity": 0.8, "blur": 0.01, "x": 0, "y": 0}, **stroke()))
    L += eyes(cid, b, 0.475, 0.19, 0.2, 0.1, look, pupil="#0b2545", white="#00f5d4")
    for i in range(5):
        add(R(f"pb_led{i}", 0.48 + i * 0.038, 0.32, 0.024, 0.04, "#00f5d4", anchor_y=0.5,
              scale_y=talk(cid, 0.2, 1.4 + 0.4 * (2 - abs(2 - i))), glow={"color": "#00f5d4", "opacity": 0.7, "blur": 0.006, "x": 0, "y": 0}))
    add(P("pb_shine", [["M", 0.47, 0.37], ["L", 0.53, 0.165], ["L", 0.57, 0.165], ["L", 0.51, 0.37], ["Z"]], "#ffffff", opacity=0.18))
    add(P("pb_blade", [["M", 0.76, 0.38], ["Q", 0.97, 0.45, 1.0, 0.92], ["L", 0.76, 0.92], ["Z"]], "#adb5bd", **stroke()))
    add(P("pb_bladeshine", [["M", 0.79, 0.43], ["Q", 0.93, 0.5, 0.96, 0.85], ["L", 0.93, 0.85], ["Q", 0.9, 0.52, 0.79, 0.46], ["Z"]], "#ffffff", opacity=0.35))
    if sash:
        add(P("pb_sash", [["M", 0.09, 0.47], ["L", 0.18, 0.42], ["L", 0.75, 0.75], ["L", 0.66, 0.8], ["Z"]], "#fb8500", **stroke()))
    return L


def bear(cid, c, x, y_home, motion, look):
    """Gerald: a big brown pear of a bear; his head turns (sniff, head shake), his mouth goes with his track (growls too)."""
    a = BW * W / (BH * H)
    b, hd = f"{cid}_b", f"{cid}_head"
    fur, belly = c["fur"], c["belly"]
    L = [{**G(cid, None, 0, 0, BW, BH, anchor_y=1.0), **motion.root(y_home)},
         E(f"{cid}_shadow", 0.04, 0.96, 0.92, 0.05, "#000000", opacity=0.2, parent=cid),
         G(b, cid, 0, talk(cid, 0, -0.03), 1, 1, anchor_y=1.0)]
    add = lambda layer: L.append({**layer, "parent": layer.get("parent", b)})  # noqa
    ex_l, ex_r = motion.arms(150, -150)
    for side, lx in (("L", 0.16), ("R", 0.6)):
        add(P(f"{cid}_leg{side}", [["M", lx, 0.8], ["L", lx + 0.24, 0.8], ["L", lx + 0.25, 0.97], ["Q", lx + 0.12, 1.0, lx - 0.01, 0.97], ["Z"]], fur, **stroke()))
        add(E(f"{cid}_pad{side}", lx + 0.04, 0.93, 0.16, 0.05, shade(fur, 0.35)))
    for side, px, ex, gesture in (("L", 0.25, ex_l, talk(cid, 14, 30)), ("R", 0.75, ex_r, talk(cid, -14, -30))):
        o, i = f"{cid}_arm{side}", f"{cid}_arm{side}_t"
        L += [G(o, b, px - 0.11, 0.42, 0.22, 0.4, anchor_x=0.5, anchor_y=0.1, rotation=ex), G(i, o, 0, 0, 1, 1, anchor_x=0.5, anchor_y=0.1, rotation=gesture),
              P(f"{i}_fur", [["M", 0.2, 0.12], ["Q", 0.2, -0.02, 0.5, -0.02], ["Q", 0.8, -0.02, 0.8, 0.12], ["L", 0.86, 0.82], ["Q", 0.5, 1.02, 0.14, 0.82], ["Z"]],
                fur, parent=i, **stroke()),
              P(f"{i}_claws", [["M", 0.26, 0.88], ["L", 0.3, 1.0], ["L", 0.36, 0.9], ["L", 0.44, 1.03], ["L", 0.5, 0.92], ["L", 0.58, 1.03], ["L", 0.64, 0.9],
                               ["L", 0.7, 1.0], ["L", 0.74, 0.88], ["Z"]], "#f8f9fa", parent=i, **stroke())]
    add(P(f"{cid}_body", [["M", 0.5, 0.3], ["C", 0.2, 0.3, 0.08, 0.5, 0.1, 0.75], ["C", 0.12, 0.95, 0.3, 0.99, 0.5, 0.99], ["C", 0.7, 0.99, 0.88, 0.95, 0.9, 0.75],
                          ["C", 0.92, 0.5, 0.8, 0.3, 0.5, 0.3], ["Z"]], fur, **stroke()))
    add(P(f"{cid}_bodyshade", [["M", 0.62, 0.33], ["C", 0.84, 0.4, 0.92, 0.6, 0.89, 0.78], ["C", 0.86, 0.93, 0.74, 0.97, 0.62, 0.98], ["C", 0.76, 0.85, 0.8, 0.6, 0.62, 0.33],
                               ["Z"]], shade(fur, 0.2), opacity=0.7))
    add(P(f"{cid}_belly", oval(0.5, 0.72, 0.24, 0.22), belly, **stroke()))
    add(P(f"{cid}_tuft", [["M", 0.4, 0.4], ["L", 0.44, 0.47], ["L", 0.48, 0.41], ["L", 0.52, 0.48], ["L", 0.56, 0.41], ["L", 0.6, 0.46], ["L", 0.62, 0.4]], "#00000000",
          stroke=shade(fur, 0.4), stroke_width=0.0016))
    L.append(G(hd, b, 0.18, 0.02, 0.64, 0.36, anchor_x=0.5, anchor_y=0.92, rotation=clean_prop(motion.head)))
    ah = a * 0.64 / 0.36
    for side, ex_ in (("L", 0.18), ("R", 0.82)):
        L += [P(f"{hd}_ear{side}", oval(ex_, 0.16, 0.13 / ah, 0.13), fur, parent=hd, **stroke()),
              P(f"{hd}_inner{side}", oval(ex_, 0.17, 0.07 / ah, 0.07), belly, parent=hd)]
    L += [P(f"{hd}_face", oval(0.5, 0.52, 0.42 / ah, 0.42, 1.08), fur, parent=hd, **stroke()),
          P(f"{hd}_muzzle", oval(0.5, 0.7, 0.2 / ah, 0.17), belly, parent=hd, **stroke()),
          P(f"{hd}_nose", [["M", 0.43, 0.6], ["Q", 0.5, 0.56, 0.57, 0.6], ["Q", 0.57, 0.66, 0.5, 0.7], ["Q", 0.43, 0.66, 0.43, 0.6], ["Z"]], "#1a1a1a", parent=hd),
          E(f"{hd}_noseshine", 0.47, 0.59, 0.03, 0.025, "#ffffff", opacity=0.6, parent=hd)]
    L += eyes(cid, hd, 0.32, 0.3, 0.36, 0.16, look)
    L += mouth(cid, hd, 0.42, 0.74, 0.16, 0.08, big=1.4)
    L += [P(f"{hd}_fangs", [["M", 0.44, 0.745], ["L", 0.46, 0.79], ["L", 0.48, 0.745], ["M", 0.52, 0.745], ["L", 0.54, 0.79], ["L", 0.56, 0.745]], "#ffffff",
            parent=hd, opacity=talk(cid, 0, 4))]
    return L


def clean_prop(keys):
    keys = clean(keys)
    return keys[0][1] if len(keys) == 1 else {"keyframes": keys}


def prop(kind, cid, show=1):
    """Something held in the right hand."""
    h = f"{cid}_armR_t"
    if kind == "mic":
        return [R(f"{cid}_mic_h", 0.44, 0.6, 0.12, 0.42, "#212529", parent=h, opacity=show, **stroke()),
                P(f"{cid}_mic", oval(0.5, 0.56, 0.17, 0.09), "#adb5bd", parent=h, opacity=show, **stroke())]
    if kind == "phone":
        return [R(f"{cid}_phone", 0.24, 0.5, 0.52, 0.3, "#212529", radius=0.2, parent=h, rotation=-10, **stroke()),
                R(f"{cid}_phone_s", 0.28, 0.53, 0.44, 0.24, "#4cc9f0", parent=h, rotation=-10),
                E(f"{cid}_rec", 0.32, 0.55, 0.08, 0.05, "#d00000", parent=h, opacity=kf([0, 1], [0.5, 0], [1, 1], loop=1))]
    if kind == "magnifier":
        return [R(f"{cid}_mag_h", 0.45, 0.75, 0.1, 0.35, "#6f4518", parent=h, **stroke()),
                P(f"{cid}_mag", oval(0.5, 1.22, 0.28, 0.13), "#bde0fe", parent=h, opacity=0.55, stroke="#495057", stroke_width=0.004)]
    if kind == "flashlight":
        return [P(f"{cid}_beam", [["M", 0.45, 0.95], ["L", 0.55, 0.95], ["L", 1.6, 2.6], ["L", -0.6, 2.6], ["Z"]], "#fff3b0", parent=h, opacity=0.28),
                R(f"{cid}_torch", 0.38, 0.72, 0.24, 0.28, "#343a40", parent=h, **stroke())]
    if kind == "shovel":
        return [R(f"{cid}_shaft", 0.44, 0.3, 0.12, 1.1, "#8d5524", parent=h, opacity=show, **stroke()),
                P(f"{cid}_blade", [["M", 0.1, 1.35], ["L", 0.9, 1.35], ["L", 0.82, 1.75], ["Q", 0.5, 1.85, 0.18, 1.75], ["Z"]], "#adb5bd", parent=h, opacity=show,
                  **stroke())]
    return []


def person(cid, c, x, ground, motion, look, **kw):
    if c["kind"] == "bear":
        return bear(cid, c, x, ground - BH, motion, look)
    if c["kind"] == "kid":
        return kid(cid, c, x, ground - KH, motion, look, **kw)
    if c["kind"] == "adult":
        return adult(cid, c, x, ground - AH, motion, look)
    return robot(x, ground - RH, motion, look, **kw)


def width_of(kind):
    return {"kid": KW, "adult": AW, "robot": RW, "bear": BW}[kind]
