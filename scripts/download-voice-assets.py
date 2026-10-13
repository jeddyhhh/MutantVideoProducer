"""Fetch pinned catalog assets, verifying size and SHA-256 before installation."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument("--output", default="deps/voice-assets")
parser.add_argument("--catalog", default=str(Path(__file__).resolve().parents[1] / "data/voice/catalog.json"))
parser.add_argument("--voice-id", help="Only this voice plus the shared model (default all voices)")
args = parser.parse_args()
catalog = json.loads(Path(args.catalog).read_text(encoding="utf-8"))
if "assets" in catalog:
    assets = catalog["assets"]
else:
    voices = [v for v in catalog["voices"] if not args.voice_id or v["id"] == args.voice_id]
    if not voices:
        parser.error("Unknown voice ID")
    assets = [catalog["model"], *voices]
root = Path(args.output).resolve()
root.mkdir(parents=True, exist_ok=True)
for asset in assets:
    path = (root / asset["file"]).resolve()
    if not path.is_relative_to(root):
        raise ValueError("Invalid asset path")
    if path.is_file() and path.stat().st_size == asset["size"] and hashlib.sha256(path.read_bytes()).hexdigest() == asset["sha256"]:
        continue
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".part")
    digest = hashlib.sha256()
    size = 0
    try:
        with urllib.request.urlopen(asset["url"], timeout=120) as response, temporary.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                output.write(chunk); digest.update(chunk); size += len(chunk)
        if size != asset["size"] or digest.hexdigest() != asset["sha256"]:
            raise ValueError(f"Download checksum failed: {asset['file']}")
        temporary.replace(path)
        print(f"Verified {asset['file']}", flush=True)
    finally:
        temporary.unlink(missing_ok=True)
