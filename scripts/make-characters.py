"""Builds the character library (data/characters/*.json) and the Animation: characters preset (src/core/CharacterPreset.cpp)
from character_rig.py: cut-out characters whose mouths open with their voice and take the shape of the words, whose arms
and bodies move as they talk, who blink, with their main colours as settings.

python scripts/make-characters.py"""
import json, os
import character_rig as r

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "data", "characters")
COLOURS = ("hat", "band", "pom", "jacket", "mitten", "skin", "hair", "fringe", "suit", "shirt", "tie", "pants", "cap", "fur", "belly", "towel",
           "stripe")
SHADED = {"head", "jacket", "hat", "torso", "body", "belly", "head_face"}

LIBRARY = {
    "kid": ("Kid", dict(kind="kid", hat="#2b59c3", band="#ffd23f", pom="#ffd23f", jacket="#d7263d", mitten="#1b998b", skin="#f6d2ad",
                        chubby=True, lids=True)),
    "kid-hair": ("Kid with long hair", dict(kind="kid", hat="#7b2cbf", band="#f8f9fa", pom="#f8f9fa", jacket="#2ec4b6", mitten="#ff9f1c",
                                            skin="#e8b896", hair="#5a3420", lashes=True)),
    "kid-flaps": ("Kid with ear flaps", dict(kind="kid", hat="#3a7d44", band="#e9c46a", pom=None, flaps=True, jacket="#f4a261",
                                             mitten="#264653", skin="#f6d2ad", fringe="#c1440e")),
    "kid-hood": ("Kid in a hood", dict(kind="kid", hat="#ffd60a", band="#ffd60a", pom=None, hood=True, jacket="#ffd60a", mitten="#1d3557",
                                       skin="#c68b59")),
    "man": ("Man with a cap and beard", dict(kind="adult", suit="#8a5a44", shirt="#e9edc9", tie=None, hair="#6f4e37", style="cap",
                                             cap="#2a9d8f", beard=True, skin="#f1c27d", pants="#264653")),
    "teacher": ("Teacher", dict(kind="adult", suit="#6b705c", shirt="#f1faee", tie="#9d0208", hair="#bcb8b1", style="bald", glasses=True,
                                skin="#f6d2ad", pants="#3d405b")),
    "woman": ("Woman", dict(kind="adult", suit="#c9184a", shirt="#ffe5ec", tie=None, hair="#e9c46a", style="bouffant", skin="#f1c27d",
                            pants="#590d22", lipstick=True)),
    "bear": ("Bear", dict(kind="bear", fur="#7f4f24", belly="#c89f72")),
    "towel": ("Terry the towel", dict(kind="towel", towel="#4ea8de", stripe="#ffffff")),
}


def build(cid, c, x, ground):
    m = r.Motion(x, r.width_of(c["kind"]), [], 10)
    return r.person(cid, c, x, ground, m, 0)


def settle(value):
    """A keyframed value that never changes, as a plain number."""
    if isinstance(value, dict) and len(value.get("keyframes", [])) == 1 and len(value) == 1:
        return value["keyframes"][0][1]
    return value


def colour_variables(layers, owner, c, prefix=""):
    variables = {}
    for layer in layers:
        if not layer["id"].startswith(owner):
            continue
        for key in ("fill", "stroke"):
            value = layer.get(key)
            if not isinstance(value, str):
                continue
            match = next((k for k in COLOURS if c.get(k) == value), None)
            if match and key == "fill":
                variables[prefix + match] = value
                layer[key] = {"variable": prefix + match}
    return variables


def rename_mood(value, to):
    """The rig's mood variable (voice_mood) under another name."""
    if isinstance(value, dict):
        if value.get("variable") == "voice_mood":
            value = {**value, "variable": to}
        return {k: rename_mood(v, to) for k, v in value.items()}
    if isinstance(value, list):
        return [rename_mood(v, to) for v in value]
    return value


def character(name, c):
    cid = "Voice"  # (its binding is then "voice"; the ids lose the prefix below)
    layers = build(cid, c, 0.0, r.KH if c["kind"] == "kid" else r.AH if c["kind"] == "adult" else r.TH if c["kind"] == "towel" else r.BH)
    variables = colour_variables(layers, cid, c)
    variables["mood"] = "neutral"  # (expressions: neutral, angry, sad, surprised, smug)
    layers = rename_mood(layers, "mood")
    out = []
    for layer in layers:
        layer = dict(layer)
        for key in ("x", "y", "rotation"):
            if key in layer:
                layer[key] = settle(layer[key])
        rename = lambda i: "root" if i == cid else i[len(cid) + 1:] if i.startswith(cid + "_") else i  # noqa
        layer["id"] = rename(layer["id"])
        if "parent" in layer:
            layer["parent"] = rename(layer["parent"])
        if layer["id"] in SHADED:
            layer["shading"] = 0.35
        out.append(layer)
    root = out[0]
    root["x"], root["y"] = 0, 0
    return {"version": 1, "name": name, "kind": c["kind"], "binding": "voice", "width": root.get("width"), "height": root.get("height"),
            "variables": variables, "layers": out}


def preset():
    """Animation: characters: two kids in a close two-shot, voice_a and voice_b."""
    GROUND = 0.92
    VARS = {}
    a = dict(LIBRARY["kid"][1])
    b = dict(LIBRARY["kid-flaps"][1])
    r.KW, r.KH = 0.26, 0.7
    layers = [{"type": "rect", "id": "background", "name": "Background", "x": 0, "y": 0, "width": 1, "height": 1, "fill": "#8ecae6",
               "gradient": {"type": "linear", "x1": 0.5, "y1": 0, "x2": 0.5, "y2": 1, "stops": [[0, "#8ecae6"], [1, "#e0fbfc"]]}},
              {"type": "rect", "id": "ground", "name": "Ground", "x": 0, "y": 0.82, "width": 1, "height": 0.18, "fill": "#f4f7fb"}]
    for cid, c, x, prefix in (("voice_a", a, 0.16, "a_"), ("voice_b", b, 0.58, "b_")):
        part = build(cid, c, x, GROUND)
        part[0]["name"] = "Character A (voice_a)" if cid == "voice_a" else "Character B (voice_b)"
        VARS.update(colour_variables(part, cid, c, prefix))
        VARS[f"{cid}_mood"] = "neutral"
        for layer in part:
            if layer["id"].replace(cid + "_", "") in SHADED:
                layer["shading"] = 0.35
        layers += part
    r.KW, r.KH = 0.14, 0.38
    scene = {"version": 2, "variables": VARS, "layers": layers}
    text = json.dumps(scene, separators=(",", ":"))
    chunks = [text[i:i + 12000] for i in range(0, len(text), 12000)]
    body = "\n".join(f'    R"json({c})json"' for c in chunks)
    src = f"""// Generated by scripts/make-characters.py from the cut-out rig (scripts/character_rig.py): two characters whose mouths,
// arms and bodies follow the audio bindings voice_a and voice_b (their mouths take the shape of the words), colours as
// scene variables.
#include "core/MotionGraphics.h"
#include <QJsonDocument>

namespace motion {{
QJsonObject charactersScene()
{{
    static const QJsonObject scene = QJsonDocument::fromJson(QByteArray(
{body})).object();
    return scene;
}}
}} // namespace motion
"""
    open(os.path.join(ROOT, "src", "core", "CharacterPreset.cpp"), "w", encoding="utf-8", newline="\n").write(src)
    return len(layers)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for key, (name, c) in LIBRARY.items():
        ch = character(name, c)
        json.dump(ch, open(os.path.join(OUT, f"{key}.json"), "w", encoding="utf-8", newline="\n"), separators=(",", ":"))
        print(key, len(ch["layers"]), "layers", sorted(ch["variables"]))
    print("preset", preset(), "layers")
