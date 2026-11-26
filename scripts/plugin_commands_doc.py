"""Writes docs/plugin-commands.md: every command mvp.call takes (the AI Control tools), with its arguments, from the
program itself (MutantVideoProducer --mcp-stdio answers tools/list when nothing listens on the port).

    python scripts/plugin_commands_doc.py [path/to/MutantVideoProducer.exe]

Run it after adding or changing a tool, and commit the page with the change."""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/Release/bin/MutantVideoProducer.exe"
OUT = ROOT / "docs/plugin-commands.md"

# What each group of commands is for, in the order the page lists them (a tool not named here goes under "Other").
GROUPS = [
    ("Project and media", ["get_project", "new_project", "open_project", "save_project", "collect_project", "set_project_settings",
                           "import_media", "media", "replace_media", "youtube_clip", "user_guide"]),
    ("Placing and editing events", ["add_media_to_timeline", "add_text", "edit_event", "split", "delete_events", "duplicate_events",
                                    "group", "paste_attributes", "freeze_frame", "cut_time", "add_track", "edit_track", "delete_track",
                                    "envelope", "compound_sequence", "multicam", "undo", "redo"]),
    ("Markers, beats and story", ["add_marker", "remove_markers", "detect_beats", "beat_montage", "directors_dice", "scene_changes",
                                  "story_chapters"]),
    ("Effects", ["list_effects", "add_effect", "set_effect", "remove_effect", "presets", "node_effect", "match_colour", "stabilize",
                 "track_motion", "auto_reframe"]),
    ("Graphics and animation", ["graphics_overlay", "waveform_graphic"]),
    ("Sound, voices and captions", ["list_sound_effects", "normalize_audio", "audio_finishing", "separate_stems", "captions", "list_voices",
                                    "generate_voice", "generate_dialogue", "voice_status", "list_vst3_plugins", "ai_components"]),
    ("Looking and playing", ["get_frame", "transport"]),
    ("Rendering and delivery", ["render", "render_status", "render_queue", "deliver", "youtube_publish"]),
    ("Live", ["live_status", "live_action", "set_live_source", "set_tempo", "set_pad", "save_scene", "live_setlist", "live_switcher",
              "get_live_frame"]),
]
NOT_FOR_PLUGINS = {"new_project", "open_project", "undo", "redo"}


def tools():
    message = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}) + "\n"
    out = subprocess.run([str(EXE), "--mcp-stdio", "--port", "1"], input=message, capture_output=True, text=True, timeout=60).stdout
    return json.loads(out.splitlines()[0])["result"]["tools"]


def kind(schema):
    t = schema.get("type", "")
    if "enum" in schema:
        return " / ".join(f"`{v}`" for v in schema["enum"])
    if t == "array":
        return f"list of {schema.get('items', {}).get('type', 'values')}"
    return t or "any"


def cell(text):
    return str(text).replace("|", "\\|").replace("<", "&lt;").replace("\n", " ")


def main():
    listed = {t["name"]: t for t in tools()}
    named = [n for _, names in GROUPS for n in names]
    groups = GROUPS + [("Other", sorted(n for n in listed if n not in named))]
    lines = ["# Plugin Commands",
             "",
             "Every command a plugin's script can run with `mvp.call(name, arguments)`; they are also the commands",
             "[AI Control](ai-control.md) gives an assistant. Times are in seconds; ids (`event_id`, `media_id`,",
             "`effect_id`) come from `get_project`; track indexes count from 0 at the top. Each command answers with an",
             "object, and throws an `Error` with the reason when it can't do what it was asked. See [Plugins](plugins.md).",
             "",
             "This page is made from the program by `scripts/plugin_commands_doc.py`; in a script, `mvp.tools()` gives the",
             "same list.",
             ""]
    for title, names in groups:
        names = [n for n in names if n in listed]
        if not names:
            continue
        lines += [f"## {title}", ""]
        for n in names:
            t = listed[n]
            schema = t.get("inputSchema", {})
            props, required = schema.get("properties", {}), set(schema.get("required", []))
            lines += [f"### {n}", ""]
            if n in NOT_FOR_PLUGINS:
                lines += ["*Not available to plugins (a plugin works on the open project, in one undo step).*", ""]
            lines += [cell(t.get("description", "")), ""]
            if props:
                lines += ["| Argument | Type | What it is |", "|---|---|---|"]
                for p, s in props.items():
                    lines.append(f"| `{p}`{' (required)' if p in required else ''} | {cell(kind(s))} | {cell(s.get('description', ''))} |")
                lines.append("")
            else:
                lines += ["No arguments.", ""]
    OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"{OUT}: {len(listed)} commands")


if __name__ == "__main__":
    main()
