"""Stem separation with Demucs (Meta's Hybrid Transformer Demucs, MIT licence), offline after its model is downloaded.

python -I worker.py request.json, where the request is
{"input": a 44.1 kHz stereo WAV, "output": a folder, "stems": 2 or 4, "model": "htdemucs" | "htdemucs_ft",
 "device": "cpu" | "cuda", "models": the folder Demucs keeps its downloaded model in}.
Prints one JSON object per line: {"progress": 0..1, "message": ...}, then {"done": true, "stems": {name: path}}, or
{"error": message} (and exits 1). 2 stems: voice and background (everything else); 4: vocals, drums, bass and other.
"""
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / "vendor"))  # (Demucs and what it needs: vendor/VENDOR.md)

MODELS = ("htdemucs", "htdemucs_ft")
RATE = 44100


def emit(**message):
    print(json.dumps(message), flush=True)


def validate(request):
    """The request, checked (raises ValueError with what is wrong)."""
    source = Path(request.get("input", ""))
    if not source.is_file():
        raise ValueError(f"There is no sound file at {source}")
    if int(request.get("stems", 2)) not in (2, 4):
        raise ValueError("Separate into 2 stems (voice and background) or 4 (vocals, drums, bass, other)")
    if request.get("model", "htdemucs") not in MODELS:
        raise ValueError(f"The model is one of {', '.join(MODELS)}")
    if request.get("device", "cpu") not in ("cpu", "cuda"):
        raise ValueError("The device is cpu or cuda")
    if not request.get("output"):
        raise ValueError("Give an output folder")
    return request


def chunks(frames, rate=RATE, seconds=10.0, overlap=1.0):
    """[start, end) pieces of about `seconds` that overlap by `overlap` (crossfaded when put back), covering frames."""
    size, lap = int(seconds * rate), int(overlap * rate)
    if frames <= size:
        return [(0, frames)]
    out, start = [], 0
    while start < frames:
        end = min(frames, start + size)
        out.append((start, end))
        if end == frames:
            break
        start = end - lap
    return out


def crossfade(length, lap, first, last):
    """Weights for a piece of `length` frames: rising over its first `lap` (unless first), falling over its last (unless
    last). Where two pieces overlap by `lap` their weights add up to 1."""
    weights = [1.0] * length
    for i in range(min(lap, length)):
        if not first:
            weights[i] = (i + 0.5) / lap
        if not last:
            weights[length - 1 - i] = min(weights[length - 1 - i], (i + 0.5) / lap)
    return weights


def names(stems):
    return ("voice", "background") if stems == 2 else ("vocals", "drums", "bass", "other")


def main(request_file):
    request = validate(json.loads(Path(request_file).read_text(encoding="utf-8")))
    stems = int(request.get("stems", 2))
    if request.get("models"):
        os.environ["TORCH_HOME"] = request["models"]
    emit(progress=0, message="Loading the separation engine")
    import numpy as np
    import soundfile as sf
    import torch
    from demucs.apply import apply_model
    from demucs.pretrained import get_model

    device = "cuda" if request.get("device") == "cuda" and torch.cuda.is_available() else "cpu"
    emit(progress=0.02, message=f"Loading the {request.get('model', 'htdemucs')} model (downloaded the first time)")
    model = get_model(request.get("model", "htdemucs"))
    model.eval()
    audio, rate = sf.read(request["input"], dtype="float32", always_2d=True)
    if rate != model.samplerate:
        raise ValueError(f"The sound must be {model.samplerate} Hz (it is {rate})")
    if audio.shape[1] == 1:
        audio = np.repeat(audio, 2, axis=1)
    audio = audio[:, :2].T  # (channels, frames)
    frames = audio.shape[1]
    sources = list(model.sources)  # drums, bass, other, vocals
    result = np.zeros((len(sources), 2, frames), dtype=np.float32)
    pieces = chunks(frames, model.samplerate)
    lap = int(model.samplerate)
    for n, (a, b) in enumerate(pieces):
        emit(progress=0.05 + 0.9 * n / len(pieces), message=f"Separating {n + 1} of {len(pieces)} on the {device.upper()}")
        mix = torch.from_numpy(np.ascontiguousarray(audio[:, a:b]))
        ref = mix.mean(0)
        scale = float(ref.std()) or 1.0
        with torch.no_grad():
            out = apply_model(model, ((mix - ref.mean()) / scale)[None], device=device, split=True, overlap=0.25, progress=False)[0]
        out = (out * scale + ref.mean()).cpu().numpy()
        weights = np.asarray(crossfade(b - a, lap, n == 0, n == len(pieces) - 1), dtype=np.float32)
        result[:, :, a:b] += out * weights
    folder = Path(request["output"])
    folder.mkdir(parents=True, exist_ok=True)
    base = Path(request["input"]).stem
    written = {}
    if stems == 2:
        voice = result[sources.index("vocals")]
        background = result.sum(0) - voice
        parts = {"voice": voice, "background": background}
    else:
        parts = {name: result[sources.index(name)] for name in names(4)}
    for name, data in parts.items():
        path = folder / f"{base} - {name}.wav"
        sf.write(str(path), data.T, model.samplerate, subtype="FLOAT")
        written[name] = str(path)
    emit(progress=1, message="Done")
    emit(done=True, stems=written)


if __name__ == "__main__":
    try:
        main(sys.argv[1])
    except Exception as error:  # (the editor shows it)
        emit(error=str(error))
        sys.exit(1)
