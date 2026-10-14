"""Mutant Video Producer's isolated, offline Kokoro worker. One request, one result directory.

Network access belongs to the editor's verified asset downloader, never this worker.
RVC runs in Mutant Video Producer's separate portable engine, loaded once per batch.
"""
import hashlib
import json
import math
import os
import re
from pathlib import Path
import subprocess
import sys
import time
import wave

os.environ.setdefault("OMP_NUM_THREADS", "2")
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ["HF_HUB_OFFLINE"] = "1"


def emit(**message):
    print(json.dumps(message, ensure_ascii=True), flush=True)


def cuda_library_roots():
    override = os.environ.get("MVP_CUDA_LIBRARY_DIR", "")
    return ([Path(override)] if override else []) + [
        Path(sys.executable).parent.parent / "rvc-worker" / "_internal" / "torch" / "lib",
        Path(__file__).resolve().parents[2] / "deps" / "rvc-worker-env" / "Lib" / "site-packages" / "torch" / "lib"]


def speech_session(ort, model_path, device="auto", profile=""):
    """GPU first, retaining ONNX's CPU operator fallback and a CPU-only session."""
    if device not in ("auto", "gpu", "cpu"):
        raise ValueError("Voice device must be auto, gpu or cpu")
    available = ort.get_available_providers()
    if device != "cpu" and "CUDAExecutionProvider" in available and os.name == "nt":
        # Share the bundled CUDA/cuDNN libraries with RVC. No system Toolkit is
        # required; the driver is the only machine-level dependency.
        for root in cuda_library_roots():
            if (root / "cudnn64_9.dll").is_file():
                try:
                    ort.preload_dlls(directory=str(root))
                except Exception as error:
                    emit(message=f"GPU library loading failed; CPU fallback remains available: {error}")
                break
    candidates = [] if device == "cpu" else [p for p in
        ("CUDAExecutionProvider", "DmlExecutionProvider", "CoreMLExecutionProvider") if p in available]
    for provider in candidates + ["CPUExecutionProvider"]:
        options = ort.SessionOptions()
        options.intra_op_num_threads = 2
        options.inter_op_num_threads = 1
        options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
        if provider == "DmlExecutionProvider":
            options.enable_mem_pattern = False
        if profile:
            options.enable_profiling = True
            options.profile_file_prefix = str(profile)
        try:
            providers = [provider] if provider == "CPUExecutionProvider" else [provider, "CPUExecutionProvider"]
            if provider == "CUDAExecutionProvider":
                # Narration lengths vary; exhaustive cuDNN tuning at every new
                # shape can cost more than synthesis itself.
                providers[0] = (provider, {"cudnn_conv_algo_search": "HEURISTIC"})
            session = ort.InferenceSession(str(model_path), sess_options=options, providers=providers)
            # ORT may silently disable an unavailable provider.
            actual = session.get_providers()[0]
            emit(message="Speech inference: " + actual, backend=actual)
            return session
        except Exception as error:
            if provider == "CPUExecutionProvider":
                raise
            emit(message=f"{provider} unavailable; trying fallback: {error}")


def check_cancel(folder):
    if (folder / "cancel").exists():
        raise RuntimeError("Cancelled")


def verify(path, expected):
    if not path.is_file() or path.stat().st_size != expected["size"]:
        raise ValueError(f"Missing or incomplete voice asset: {path.name}")
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != expected["sha256"]:
        raise ValueError(f"Voice asset checksum failed: {path.name}. Download it again.")


def write_wav(path, audio, sample_rate):
    import numpy as np
    samples = np.asarray(audio, dtype=np.float32).ravel()
    if not len(samples) or not np.isfinite(samples).all():
        raise ValueError("Speech engine produced empty or invalid audio")
    # Avoid clipping without boosting quiet speech or gaps.
    peak = float(np.max(np.abs(samples)))
    if peak > 0.98:
        samples *= 0.98 / peak
    with wave.open(str(path), "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(sample_rate)
        file.writeframes((np.clip(samples, -1, 1) * 32767).astype("<i2").tobytes())


def speech_parts(text, pronunciations=None, leading_pause=0, trailing_pause=0, sentence_pause=0):
    """Keep caption spelling separate from spoken pronunciation and real silent pauses."""
    if len(text) > 20000:
        raise ValueError("Narration text is too long")
    pronunciations = pronunciations or {}
    if not isinstance(pronunciations, dict) or len(pronunciations) > 500:
        raise ValueError("Use at most 500 pronunciation entries")
    for value in (leading_pause, trailing_pause, sentence_pause):
        if not isinstance(value, (int, float)) or not math.isfinite(value) or not 0 <= value <= 10:
            raise ValueError("Pauses must be between zero and ten seconds")
    for word, spoken in sorted(pronunciations.items(), key=lambda item: -len(str(item[0]))):
        if not isinstance(word, str) or not isinstance(spoken, str) or not word.strip() or len(word) > 200 or len(spoken) > 500:
            raise ValueError("Invalid pronunciation entry")
        text = re.sub(r"(?<!\w)" + re.escape(word) + r"(?!\w)", lambda match: spoken, text, flags=re.I)
        if len(text)>40000:
            raise ValueError("Expanded narration is too long")
    if sentence_pause:
        text = re.sub(r"([.!?])\s+", lambda match: match[1]+f" [pause={sentence_pause}] ", text)
    parts = [("pause", float(leading_pause))]
    cursor = 0
    for match in re.finditer(r"\[pause[=:]([0-9.]+)\]", text, flags=re.I):
        value = float(match[1])
        if not math.isfinite(value) or not 0 <= value <= 10:
            raise ValueError("Inline pauses must be between zero and ten seconds")
        parts.extend([("text", text[cursor:match.start()].strip()), ("pause", value)])
        cursor = match.end()
    parts.extend([("text", text[cursor:].strip()), ("pause", float(trailing_pause))])
    if any("[pause" in value.lower() for kind, value in parts if kind == "text"):
        raise ValueError("Use [pause=0.5] for an inline pause")
    if len(parts)>1000 or sum(value for kind,value in parts if kind=="pause")>600:
        raise ValueError("Use at most 500 speech parts and ten minutes of silence")
    return [(kind, value) for kind, value in parts if value]


def rvc_command(config, source, target):
    root = Path(config["root"]).resolve()
    python = Path(config["python"]).resolve()
    cli = root / "infer" / "cli.py"
    model = Path(config["model"]).resolve()
    index = config.get("index", "")
    if not python.is_file() or not cli.is_file() or not model.is_file() or model.suffix.lower() != ".pth":
        raise ValueError("RVC needs its Python executable, official infer/cli.py folder, and a .pth voice model.")
    rate = float(config.get("index_rate", 0))
    if not 0 <= rate <= 1 or not 0 <= float(config.get("protect", 0.33)) <= 0.5:
        raise ValueError("Invalid RVC retrieval or protection setting")
    if not -24 <= int(config.get("pitch", 0)) <= 24 or config.get("f0_method", "rmvpe") not in ("pm", "rmvpe"):
        raise ValueError("Invalid RVC pitch or pitch detector")
    if rate > 0 and (not index or not Path(index).is_file()):
        raise ValueError("Choose a matching .index file or set retrieval to zero.")
    args = [str(python), str(cli), "--model", str(model), "--input", str(source), "--output", str(target),
            "--pitch", str(int(config.get("pitch", 0))), "--f0-method", config.get("f0_method", "rmvpe"),
            "--index-rate", str(rate), "--protect", str(float(config.get("protect", 0.33))),
            "--speaker-id", str(int(config.get("speaker_id", 0))), "--overwrite"]
    if index and rate > 0:
        args += ["--index", str(Path(index).resolve())]
    return args, root


def convert_rvc(config, source, target, folder):
    args, root = rvc_command(config, source, target)
    log = folder / "rvc.log"
    with log.open("ab") as output:
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        child = subprocess.Popen(args, cwd=root, stdout=output, stderr=subprocess.STDOUT, creationflags=flags)
        try:
            while child.poll() is None:
                check_cancel(folder)
                time.sleep(0.1)
            if child.returncode or not target.is_file():
                detail = log.read_text(encoding="utf-8", errors="replace")[-2500:]
                raise RuntimeError("RVC conversion failed. Check the RVC environment and model.\n" + detail)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()


def bundled_rvc_preflight(config):
    if not Path(config.get("worker", "")).is_file():
        raise ValueError("The bundled RVC engine is missing from this build")
    model = Path(config.get("model", ""))
    if not model.is_file() or model.suffix.lower() != ".pth":
        raise ValueError("Import an exported RVC .pth voice first")
    if not -24 <= int(config.get("pitch", 0)) <= 24 or config.get("f0_method", "rmvpe") not in ("rmvpe", "pm"):
        raise ValueError("Invalid RVC pitch or pitch detector")
    if not 0 <= float(config.get("index_rate", 0)) <= 1 or not 0 <= float(config.get("protect", 0.33)) <= 0.5:
        raise ValueError("Invalid RVC retrieval or protection setting")
    if float(config.get("index_rate", 0)) > 0 and not Path(config.get("index", "")).is_file():
        raise ValueError("Choose a matching RVC index, or set retrieval to zero")
    if len(config.get("assets", [])) != 4:
        raise ValueError("Download the RVC support models first")
    for asset in config["assets"]:
        verify(Path(asset["path"]), asset)


def convert_bundled_rvc(config, inputs, folder):
    request = folder / "rvc-request.json"
    request.write_text(json.dumps({"rvc": config, "inputs": inputs}), encoding="utf-8")
    log = folder / "rvc.log"
    stream = folder / "rvc-progress.jsonl"
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    with stream.open("wb") as output, log.open("wb") as errors, stream.open("r", encoding="utf-8") as progress:
        worker_args = config.get("worker_args", [])
        if not isinstance(worker_args, list) or not all(isinstance(arg, str) for arg in worker_args):
            raise ValueError("Invalid RVC worker arguments")
        child = subprocess.Popen([config["worker"], *worker_args, str(request)], stdout=output, stderr=errors, creationflags=flags)
        last_error = ""
        try:
            pending = ""
            while True:
                check_cancel(folder)
                pending += progress.read()
                while "\n" in pending:
                    line, pending = pending.split("\n", 1)
                    try:
                        item = json.loads(line)
                    except ValueError:
                        continue
                    if "error" in item: last_error = item["error"]
                    if "backend" in item:
                        emit(engine="rvc", backend=item["backend"], message=item.get("message", "RVC inference"))
                    if "progress" in item:
                        emit(progress=0.5 + 0.5 * float(item["progress"]), message=item.get("message", "RVC conversion"))
                if child.poll() is not None:
                    # One more read picks up lines flushed just before exit.
                    pending += progress.read()
                    for line in pending.splitlines():
                        try:
                            item = json.loads(line)
                            if "error" in item: last_error = item["error"]
                            if "backend" in item:
                                emit(engine="rvc", backend=item["backend"], message=item.get("message", "RVC inference"))
                        except ValueError:
                            pass
                    break
                time.sleep(0.1)
            if child.returncode or any(not Path(item["output"]).is_file() for item in inputs):
                errors.flush()
                raise RuntimeError("RVC conversion failed: " + (last_error or log.read_text(encoding="utf-8", errors="replace")[-2500:]))
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()


def plan_timing(segments, durations, fit):
    takes = []
    previous_end = 0.0
    for segment, duration in zip(segments, durations, strict=True):
        at, end = float(segment["start"]), float(segment["end"])
        span = end - at
        if not all(math.isfinite(n) for n in (at, end, duration)) or at < 0 or span <= 0 or duration <= 0:
            raise ValueError("Invalid caption or speech timing")
        needed = duration / span
        speed = min(1.35, max(1.0, needed)) if fit else 1.0
        length = duration / speed
        start = max(at, previous_end)
        takes.append({"start": start, "length": length, "speed": speed, "text": segment["text"],
                      "overrun": length > span + 0.02, "shifted": start > at + 0.02})
        previous_end = start + length
    return takes


def main(request_path):
    import numpy as np
    import onnxruntime as ort
    from kokoro_onnx import Kokoro

    request = json.loads(Path(request_path).read_text(encoding="utf-8"))
    folder = Path(request_path).resolve().parent
    segments = request["segments"]
    if not segments or len(segments) > 10000:
        raise ValueError("No captions to speak (or too many captions)")
    model, voice = request["model"], request["voice"]
    bundled = bool(request.get("rvc")) and request["rvc"].get("engine", "bundled") != "external"
    if request.get("rvc"):
        if bundled: bundled_rvc_preflight(request["rvc"])
        else: rvc_command(request["rvc"], folder / "check.wav", folder / "check-rvc.wav")
    model_path, voice_path = Path(model["path"]), Path(voice["path"])
    emit(progress=0, message="Checking downloaded model and voice")
    verify(model_path, model)
    verify(voice_path, voice)
    check_cancel(folder)
    styles = np.fromfile(voice_path, dtype="<f4").reshape(-1, 1, 256)
    if not np.isfinite(styles).all():
        raise ValueError("Invalid voice style data")
    packed = folder / "voice.npz"
    np.savez(packed, **{voice["id"]: styles})
    device = request.get("device", os.environ.get("MVP_AI_DEVICE", "auto"))
    gpu_model = request.get("gpu_model")
    if device != "cpu" and gpu_model and "CUDAExecutionProvider" in ort.get_available_providers():
        verify(Path(gpu_model["path"]), gpu_model)
        model_path = Path(gpu_model["path"])
    elif device != "cpu" and "int8" in model_path.name:
        emit(message="GPU speech model is not installed; using the compact CPU model")
        device = "cpu"
    session = speech_session(ort, model_path, device, request.get("profile", ""))
    if session.get_providers()[0] == "CPUExecutionProvider" and model_path != Path(model["path"]):
        session = speech_session(ort, Path(model["path"]), "cpu")
    engine = Kokoro.from_session(session, str(packed))
    durations = []
    files = []
    for i, segment in enumerate(segments):
        check_cancel(folder)
        if len(segment["text"]) > 20000:
            raise ValueError("Caption text is too long")
        emit(progress=(0.5 if bundled else 1) * i / len(segments), message=f"Speaking {i + 1} of {len(segments)} ({session.get_providers()[0]})")
        parts = speech_parts(segment["text"], request.get("pronunciations"), request.get("leading_pause", 0),
                             request.get("trailing_pause", 0), request.get("sentence_pause", 0))
        chunks = []; sample_rate = 24000
        for kind, value in parts:
            check_cancel(folder)
            if kind == "pause":
                chunks.append(np.zeros(round(value * sample_rate), dtype=np.float32))
            else:
                try:
                    samples, actual_rate = engine.create(value, voice=voice["id"], lang=voice["language"], speed=float(request.get("rate", 1)))
                except Exception:
                    if session.get_providers()[0] == "CPUExecutionProvider":
                        raise
                    emit(message="GPU speech inference failed; retrying on CPU")
                    session = speech_session(ort, Path(model["path"]), "cpu")
                    engine = Kokoro.from_session(session, str(packed))
                    samples, actual_rate = engine.create(value, voice=voice["id"], lang=voice["language"], speed=float(request.get("rate", 1)))
                if actual_rate != sample_rate:
                    raise ValueError("Unexpected speech engine sample rate")
                chunks.append(samples)
        if not any(kind == "text" for kind, _ in parts):
            raise ValueError("Narration needs spoken words, not only pauses")
        audio = np.concatenate(chunks)
        source = folder / f"speech-{i:05d}.wav"
        write_wav(source, audio, sample_rate)
        output = source
        if request.get("rvc") and not bundled:
            emit(progress=i / len(segments), message=f"RVC conversion {i + 1} of {len(segments)}")
            output = folder / f"rvc-{i:05d}.wav"
            convert_rvc(request["rvc"], source, output, folder)
        files.append(output.name)
    if request.get("profile"):
        emit(profile=session.end_profiling())
    if bundled:
        inputs = [{"input": str(folder / name), "output": str(folder / f"rvc-{i:05d}.wav")} for i, name in enumerate(files)]
        convert_bundled_rvc(request["rvc"], inputs, folder)
        files = [Path(item["output"]).name for item in inputs]
    for name in files:
        check_cancel(folder)
        output = folder / name
        with wave.open(str(output), "rb") as wav:
            if wav.getnframes() == 0 or wav.getframerate() <= 0:
                raise ValueError("Voice conversion returned empty audio")
            durations.append(wav.getnframes() / wav.getframerate())
    takes = plan_timing(segments, durations, request.get("timing", "fit") == "fit")
    for take, name in zip(takes, files, strict=True):
        take["file"] = name
    check_cancel(folder)
    result = {"version": 1, "voice": voice["id"], "takes": takes, "speech_backend": session.get_providers()[0]}
    temporary = folder / "result.tmp"
    temporary.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(folder / "result.json")
    emit(progress=1, message="Speech ready")


if __name__ == "__main__":
    try:
        if len(sys.argv) != 2:
            raise ValueError("Usage: mvp-voice-worker request.json")
        main(sys.argv[1])
    except Exception as error:
        emit(error=str(error))
        sys.exit(1)
