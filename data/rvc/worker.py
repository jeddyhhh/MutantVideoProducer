"""Offline RVC v1/v2 inference using pinned official RVC-Project modules."""
import hashlib
import json
import os
from pathlib import Path
import sys
import time
from types import SimpleNamespace

os.environ["HF_HUB_OFFLINE"] = "1"
os.environ["TRANSFORMERS_OFFLINE"] = "1"
os.environ["RVC_CUDA_GRAPH"] = "0"
os.environ.setdefault("OMP_NUM_THREADS", "2")
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
sys.path.insert(0, str(Path(__file__).resolve().parent / "vendor"))


def emit(**message):
    print(json.dumps(message), flush=True)


def validate_settings(config):
    if not -24 <= int(config.get("pitch", 0)) <= 24:
        raise ValueError("RVC pitch must be from -24 to 24 semitones")
    if config.get("f0_method", "rmvpe") not in ("rmvpe", "pm"):
        raise ValueError("Choose RMVPE or Praat pitch detection")
    rate, protect = float(config.get("index_rate", 0)), float(config.get("protect", 0.33))
    if not 0 <= rate <= 1 or not 0 <= protect <= 0.5:
        raise ValueError("Invalid RVC retrieval/protection settings")
    if rate > 0 and not Path(config.get("index", "")).is_file():
        raise ValueError("Choose a matching RVC index, or set retrieval to zero")


def main(request_file):
    request = json.loads(Path(request_file).read_text(encoding="utf-8"))
    folder = Path(request_file).resolve().parent
    config = request["rvc"]
    validate_settings(config)
    os.environ["NUMBA_CACHE_DIR"] = str(folder / "numba-cache")
    emit(progress=0, message="Loading bundled RVC engine")
    import numpy as np
    import soundfile as sf
    import torch
    import librosa
    import faiss
    from infer.module.models import (SynthesizerTrnMs256NSFsid, SynthesizerTrnMs256NSFsid_nono,
                                    SynthesizerTrnMs768NSFsid, SynthesizerTrnMs768NSFsid_nono)
    from infer.vc.pipeline import Pipeline
    import infer.hubert as hubert

    torch.set_num_threads(2)
    torch.set_num_interop_threads(1)
    # Never fall back to executing arbitrary pickle objects from an imported voice.
    try:
        checkpoint = torch.load(config["model"], map_location="cpu", weights_only=True)
    except Exception as error:
        raise ValueError("This voice cannot be safely loaded as an RVC weights checkpoint. Import an exported RVC inference model with ordinary tensor weights.") from error
    if not isinstance(checkpoint, dict) or not isinstance(checkpoint.get("config"), (list, tuple)) or not isinstance(checkpoint.get("weight"), dict):
        raise ValueError("Import an exported RVC .pth voice, not a training checkpoint")
    weights = checkpoint["weight"]
    version, pitch_enabled = checkpoint.get("version", "v1"), int(checkpoint.get("f0", 1))
    classes = {("v1", 1): SynthesizerTrnMs256NSFsid, ("v1", 0): SynthesizerTrnMs256NSFsid_nono,
               ("v2", 1): SynthesizerTrnMs768NSFsid, ("v2", 0): SynthesizerTrnMs768NSFsid_nono}
    if (version, pitch_enabled) not in classes or "emb_g.weight" not in weights:
        raise ValueError("Unsupported RVC voice architecture")
    model_config = list(checkpoint["config"])
    if len(model_config) != 18:
        raise ValueError("This is not a supported exported RVC voice configuration")
    speakers = weights["emb_g.weight"].shape[0]
    model_config[-3] = speakers
    sample_rate = int(model_config[-1])
    if sample_rate not in (32000, 40000, 48000):
        raise ValueError("Unsupported RVC sample rate")
    speaker_id = int(config.get("speaker_id", 0))
    if not 0 <= speaker_id < speakers:
        raise ValueError(f"This RVC voice has speaker IDs 0 to {speakers - 1}")
    net = classes[(version, pitch_enabled)](*model_config, is_half=False)
    del net.enc_q
    loaded = net.load_state_dict(weights, strict=False)
    if loaded.missing_keys:
        raise ValueError("The voice checkpoint is missing required inference weights")
    net = net.float().eval()

    assets = config["assets"]
    for asset in assets:
        path = Path(asset["path"])
        if not path.is_file() or path.stat().st_size != asset["size"]:
            raise ValueError("RVC support model is missing: " + path.name)
        digest = hashlib.sha256()
        with path.open("rb") as file:
            for chunk in iter(lambda: file.read(1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != asset["sha256"]:
            raise ValueError("RVC support model checksum failed: " + path.name)
    paths = {Path(asset["file"]).name: Path(asset["path"]) for asset in assets}
    hubert.HUBERT_MODEL_PATH = paths["config.json"].parent
    os.environ["rmvpe_root"] = str(paths["rmvpe.pt"].parent)
    requested = config.get("device", os.environ.get("MVP_AI_DEVICE", "auto"))
    if requested not in ("auto", "gpu", "cpu"):
        raise ValueError("RVC device must be auto, gpu or cpu")
    device = "cpu"
    if requested != "cpu" and torch.cuda.is_available():
        try:
            # Exercise an actual kernel: availability alone does not guarantee
            # this build supports the installed GPU architecture.
            torch.ones(4, device="cuda").square().sum().item()
            device = "cuda"
        except Exception as error:
            emit(message=f"CUDA unavailable; using CPU: {error}")

    def load_engine(target):
        content_model = hubert.load_hubert_model(target, False)
        converter = Pipeline(sample_rate, SimpleNamespace(x_pad=1, x_query=6, x_center=38, x_max=41,
                             is_half=False, device=target))
        net.to(target)
        emit(backend=target, message="RVC inference: " +
             (torch.cuda.get_device_name(0) if target == "cuda" else "CPU"))
        return content_model, converter

    try:
        content, pipeline = load_engine(device)
    except Exception:
        if device == "cpu":
            raise
        device = "cpu"
        emit(message="GPU model loading failed; retrying RVC on CPU")
        content, pipeline = load_engine(device)
    index_path = config.get("index", "")
    if float(config.get("index_rate", 0)) > 0:
        index = faiss.read_index(index_path)
        if index.d != (256 if version == "v1" else 768) or index.ntotal < 8:
            raise ValueError("The RVC index does not match this voice's feature size or has too few entries")
        vectors = index.reconstruct_n(0, index.ntotal)
        if not np.isfinite(vectors).all():
            raise ValueError("The RVC index contains invalid feature vectors")

    inputs = request["inputs"]
    if not inputs or len(inputs) > 10000:
        raise ValueError("No audio to convert")
    for i, item in enumerate(inputs):
        if (folder / "cancel").exists():
            raise RuntimeError("Cancelled")
        started = time.perf_counter()
        emit(progress=i / len(inputs), message=f"RVC conversion {i + 1} of {len(inputs)} ({device.upper()})")
        audio, rate = sf.read(item["input"], dtype="float32", always_2d=True)
        audio = audio.mean(axis=1)
        if not len(audio) or not np.isfinite(audio).all():
            raise ValueError("Invalid input audio")
        audio = librosa.resample(audio, orig_sr=rate, target_sr=16000)
        peak = float(np.max(np.abs(audio)))
        if peak > 0.95: audio *= 0.95 / peak
        if hubert.hubert_audio_requires_normalization():
            audio = (audio - audio.mean()) / np.sqrt(audio.var() + 1e-7)
        def convert():
            with torch.inference_mode():
                return pipeline.pipeline(content, net, speaker_id, audio, [0, 0, 0], int(config.get("pitch", 0)),
                    config.get("f0_method", "rmvpe"), index_path, float(config.get("index_rate", 0)), pitch_enabled,
                    sample_rate, 0, 1.0, version, float(config.get("protect", 0.33)))
        try:
            output = convert()
        except Exception as error:
            if device == "cpu":
                raise
            emit(message=f"GPU RVC inference failed; retrying on CPU: {error}")
            import traceback
            traceback.print_exc(file=sys.stderr)
            device = "cpu"
            del content, pipeline
            net.cpu()
            torch.cuda.empty_cache()
            content, pipeline = load_engine(device)
            output = convert()
        if not len(output) or not np.isfinite(output).all():
            raise ValueError("RVC returned invalid audio")
        sf.write(item["output"], output, sample_rate, subtype="PCM_16")
        emit(backend=device, seconds=time.perf_counter()-started, message=f"RVC take {i + 1} ready ({device.upper()})")
    emit(progress=1, message="RVC conversion ready")


if __name__ == "__main__":
    try:
        if len(sys.argv) != 2: raise ValueError("Usage: mvp-rvc-worker request.json")
        main(sys.argv[1])
    except Exception as error:
        emit(error=str(error))
        import traceback
        traceback.print_exc(file=sys.stderr)
        sys.exit(1)
