"""Actual CPU RVC inference smoke test using official pretrained v2 f0G40k.

This validates inference, not the quality of any fine-tuned identity voice.
Download f0G40k.pth and configs/v1/40k.json from the pinned official upstream
to build/rvc-reference-generator.pth and build/rvc-reference-config.json first.
Run with deps/rvc-worker-env/Scripts/python.exe. Optional argv[1] is the packaged
worker executable; otherwise runs the source worker in this test environment.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import wave

import numpy as np
import soundfile as sf
import torch
torch.set_num_threads(2)

root = Path(__file__).resolve().parents[1]
folder = root / "build/rvc-smoke"
folder.mkdir(parents=True, exist_ok=True)
generator = root / "build/rvc-reference-generator.pth"
assert hashlib.sha256(generator.read_bytes()).hexdigest() == "3b2c44035e782c4b14ddc0bede9e2f4a724d025cd073f736d4f43708453adfcb"
state = torch.load(generator, map_location="cpu", weights_only=True)["model"]
cfg = json.loads((root / "build/rvc-reference-config.json").read_text())
m, d, t = cfg["model"], cfg["data"], cfg["train"]
keys = ["inter_channels", "hidden_channels", "filter_channels", "n_heads", "n_layers", "kernel_size", "p_dropout",
        "resblock", "resblock_kernel_sizes", "resblock_dilation_sizes", "upsample_rates", "upsample_initial_channel",
        "upsample_kernel_sizes", "spk_embed_dim", "gin_channels"]
model_config = [d["filter_length"] // 2 + 1, t["segment_size"] // d["hop_length"], *[m[k] for k in keys], d["sampling_rate"]]
voice = folder / "reference-voice.pth"
torch.save({"config": model_config, "weight": state, "version": "v2", "f0": 1}, voice)
catalog = json.loads((root / "data/rvc/catalog.json").read_text())
assets = [{**a, "path": str(root / "deps/rvc-assets" / a["file"])} for a in catalog["assets"]]
source = root / "build/voice-smoke/speech-00000.wav"
assert source.is_file(), "Generate a real Kokoro speech WAV before this test"
config = {"model": str(voice), "assets": assets, "pitch": 0, "index_rate": 0, "f0_method": "rmvpe"}
command = [sys.argv[1]] if len(sys.argv) > 1 else [sys.executable, str(root / "data/rvc/worker.py")]
sys.path.insert(0, str(root / "data/rvc/vendor"))
import faiss
import librosa
import infer.hubert as hubert
hubert.HUBERT_MODEL_PATH = root / "deps/rvc-assets/rvc/hubert_base"
content = hubert.load_hubert_model("cpu", False)
audio, rate = sf.read(source, dtype="float32")
audio = librosa.resample(audio, orig_sr=rate, target_sr=16000)
with torch.inference_mode():
    features = hubert.extract_hubert_features(content, torch.from_numpy(audio).unsqueeze(0), "v2")[0].numpy()
# Avoid exact zero distances, which upstream's weighted retrieval cannot handle.
features += np.random.default_rng(1234).normal(0, 0.001, features.shape).astype("float32")
index = faiss.IndexFlatL2(768)
index.add(features)
index_file = folder / "reference.index"
faiss.write_index(index, str(index_file))
del content
for detector in ("rmvpe", "pm", "indexed"):
    output = folder / (detector + ".wav")
    config["f0_method"] = "pm" if detector == "indexed" else detector
    config["pitch"] = 0 if detector == "rmvpe" else 3
    config["index"] = str(index_file) if detector == "indexed" else ""
    config["index_rate"] = 0.75 if detector == "indexed" else 0
    request = folder / (detector + "-request.json")
    inputs = [{"input": str(source), "output": str(output)}]
    if detector == "indexed": inputs.append({"input": str(source), "output": str(folder / "batch-second.wav")})
    request.write_text(json.dumps({"rvc": config, "inputs": inputs}))
    subprocess.run([*command, str(request)], check=True)
    audio, rate = sf.read(output)
    assert rate == 40000 and len(audio) > rate and np.isfinite(audio).all()
    assert np.sqrt(np.mean(audio ** 2)) > 0.0001, "Silent RVC output"
    with wave.open(str(source)) as wav:
        duration = wav.getnframes() / wav.getframerate()
    assert abs(len(audio) / rate - duration) < 0.05, "Conversion lost audio duration"
    print(f"PASS real {detector} conversion: {len(audio)/rate:.3f}s, RMS {np.sqrt(np.mean(audio**2)):.4f}", flush=True)
