"""Builds the prop library (data/props/*.json): small things a character holds, drawn in the same cut-out style as the
characters. Each prop is a group ("root") of layers in its own box, its size (width, height: fractions of the picture, at a
kid's scale) and its grip (the point of the box the hand holds, 0 to 1).

python scripts/make-props.py"""
import json, os
import character_rig as r

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "data", "props")


def outline(**p):
    return {"stroke": r.OL, "stroke_width": r.OW, **p}


def R(lid, x, y, w, h, fill, **p):
    return {"type": "rect", "id": lid, "parent": "root", "x": x, "y": y, "width": w, "height": h, "fill": fill, **p}


def E(lid, x, y, w, h, fill, **p):
    return {"type": "ellipse", "id": lid, "parent": "root", "x": x, "y": y, "width": w, "height": h, "fill": fill, **p}


def P(lid, cmds, fill, x=0, y=0, w=1, h=1, **p):
    return {"type": "path", "id": lid, "parent": "root", "x": x, "y": y, "width": w, "height": h, "commands": cmds, "fill": fill, **p}


def T(lid, words, x, y, w, h, size, fill="#111111", **p):
    return {"type": "text", "id": lid, "parent": "root", "text": words, "x": x, "y": y, "width": w, "height": h, "font_size": size, "fill": fill,
            "font": "Impact", "align": "center", **p}


PROPS = {
    "phone": ("Phone", 0.03, 0.055, (0.5, 0.7), [
        R("body", 0, 0, 1, 1, "#212529", radius=0.25, shading=0.4, **outline()),
        R("screen", 0.12, 0.1, 0.76, 0.75, "#4cc9f0", radius=0.1, glow={"color": "#4cc9f0", "opacity": 0.6, "blur": 0.004, "x": 0, "y": 0})]),
    "megaphone": ("Megaphone", 0.09, 0.06, (0.25, 0.85), [
        P("cone", [["M", 0.25, 0.35], ["L", 1, 0], ["L", 1, 1], ["L", 0.25, 0.65], ["Z"]], "#e63946", shading=0.4, **outline()),
        R("mouth", 0, 0.3, 0.28, 0.4, "#f8f9fa", radius=0.3, **outline()),
        R("handle", 0.2, 0.6, 0.1, 0.4, "#212529", radius=0.3, **outline())]),
    "shovel": ("Shovel", 0.05, 0.3, (0.5, 0.35), [
        R("handle", 0.42, 0, 0.16, 0.72, "#7f5539", radius=0.3, **outline()),
        R("grip", 0.25, 0, 0.5, 0.06, "#7f5539", radius=0.5, **outline()),
        P("blade", [["M", 0, 0.7], ["L", 1, 0.7], ["L", 0.9, 1], ["L", 0.1, 1], ["Z"]], "#adb5bd", shading=0.5, **outline())]),
    "lunchbox": ("Lunchbox", 0.08, 0.07, (0.5, 0.08), [
        P("handle", [["M", 0.3, 0.3], ["Q", 0.5, -0.1, 0.7, 0.3]], "#00000000", stroke="#212529", stroke_width=0.004),
        R("box", 0, 0.28, 1, 0.72, "#3a86ff", radius=0.15, shading=0.5, **outline()),
        R("lid", 0, 0.28, 1, 0.18, "#4361ee", radius=0.2, **outline())]),
    "mitten": ("Mitten", 0.04, 0.05, (0.5, 0.85), [
        P("mitt", [["M", 0.2, 1], ["L", 0.2, 0.35], ["C", 0.2, 0, 0.8, 0, 0.8, 0.35], ["L", 0.8, 0.5], ["L", 1, 0.4], ["C", 1.1, 0.55, 0.95, 0.7, 0.8, 0.75],
                   ["L", 0.8, 1], ["Z"]], "#d00000", shading=0.4, **outline()),
        R("cuff", 0.15, 0.82, 0.7, 0.18, "#f8f9fa", radius=0.3, **outline())]),
    "clapperboard": ("Clapperboard", 0.1, 0.08, (0.12, 0.85), [
        R("slate", 0, 0.3, 1, 0.7, "#212529", **outline()),
        T("take", "SCENE 1  TAKE 1", 0.05, 0.5, 0.9, 0.3, 0.012, fill="#f8f9fa"),
        R("clapper", 0, 0.08, 1, 0.22, "#f8f9fa", rotation=-8, anchor_x=0, anchor_y=1, **outline()),
        P("stripes", [c for i in range(5) for c in (["M", 0.1 + i * 0.2, 0], ["L", 0.2 + i * 0.2, 0], ["L", 0.1 + i * 0.2, 1], ["L", 0 + i * 0.2, 1], ["Z"])],
          "#212529", 0, 0.08, 1, 0.22, rotation=-8, anchor_x=0, anchor_y=1)]),
    "mug": ("Mug", 0.04, 0.05, (0.92, 0.5), [
        E("handle", 0.6, 0.2, 0.4, 0.5, "#00000000", stroke="#f8f9fa", stroke_width=0.006),
        R("cup", 0, 0, 0.75, 1, "#f8f9fa", radius=0.15, shading=0.4, **outline()),
        R("coffee", 0.06, 0.05, 0.63, 0.12, "#6f4518", radius=0.4)]),
    "sign": ("Sign", 0.12, 0.24, (0.5, 0.8), [
        R("stick", 0.45, 0.35, 0.1, 0.65, "#7f5539", **outline()),
        R("board", 0, 0, 1, 0.42, "#fefae0", shading=0.3, **outline()),
        T("words", "WOW", 0.05, 0.08, 0.9, 0.3, 0.04, fill="#d00000")]),
}


def prop(name, w, h, grip, layers):
    return {"version": 1, "name": name, "width": w, "height": h, "grip": list(grip),
            "layers": [{"type": "group", "id": "root", "name": name, "x": 0, "y": 0, "width": 1, "height": 1}] + layers}


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for key, (name, w, h, grip, layers) in PROPS.items():
        json.dump(prop(name, w, h, grip, layers), open(os.path.join(OUT, f"{key}.json"), "w", encoding="utf-8", newline="\n"), separators=(",", ":"))
        print(key, len(layers), "layers")
