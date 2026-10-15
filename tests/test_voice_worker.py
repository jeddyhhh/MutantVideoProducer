"""Contract checks for timing, downloads and the official RVC CLI adapter.

The fake CLI tests argument handling and failure/cancellation, not model quality.
Real Kokoro and editor integration are tested by mvp_voice_test.exe.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
import wave
from unittest.mock import patch
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location("voice_worker", Path(__file__).resolve().parents[1] / "data/voice/worker.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class TimingTest(unittest.TestCase):
    def test_downloaded_cuda_libraries_are_located_outside_the_application(self):
        with patch.dict(worker.os.environ, {"MVP_CUDA_LIBRARY_DIR": "/downloaded/rvc-version/_internal/torch/lib"}):
            self.assertEqual(worker.cuda_library_roots()[0], Path("/downloaded/rvc-version/_internal/torch/lib"))

    def test_failed_gpu_library_loading_still_allows_cpu_inference(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'cudnn64_9.dll').write_bytes(b'fixture')
            class FakeOrt:
                ExecutionMode = SimpleNamespace(ORT_SEQUENTIAL=0)
                def get_available_providers(self): return ['CUDAExecutionProvider', 'CPUExecutionProvider']
                def preload_dlls(self, **kwargs): raise RuntimeError('missing dependency')
                def SessionOptions(self): return SimpleNamespace()
                def InferenceSession(self, path, sess_options, providers):
                    if providers[0] != 'CPUExecutionProvider': raise RuntimeError('GPU unavailable')
                    return SimpleNamespace(get_providers=lambda: ['CPUExecutionProvider'])
            with patch.object(worker, 'os', SimpleNamespace(name='nt', environ={})), \
                 patch.object(worker, 'cuda_library_roots', return_value=[root]), patch.object(worker, 'emit'):
                self.assertEqual(worker.speech_session(FakeOrt(), root / 'model.onnx').get_providers(), ['CPUExecutionProvider'])

    def test_cuda_speech_prefers_its_own_libraries_over_rvc(self):
        calls = []
        class FakeOrt:
            ExecutionMode = SimpleNamespace(ORT_SEQUENTIAL=0)
            def get_available_providers(self): return ['CUDAExecutionProvider', 'CPUExecutionProvider']
            def preload_dlls(self, **kwargs): calls.append(kwargs)
            def SessionOptions(self): return SimpleNamespace()
            def InferenceSession(self, path, sess_options, providers):
                return SimpleNamespace(get_providers=lambda: ['CUDAExecutionProvider'])
        with tempfile.TemporaryDirectory() as directory:
            rvc = Path(directory)
            (rvc / 'cudnn64_9.dll').write_bytes(b'fixture')
            with patch.object(worker, 'os', SimpleNamespace(name='nt', environ={})), patch.object(worker, 'emit'), \
                 patch.object(worker, 'cuda_library_roots', return_value=[rvc]):
                with patch.object(worker, 'own_cuda_libraries', return_value=True):
                    worker.speech_session(FakeOrt(), rvc / 'model.onnx', 'gpu')
                with patch.object(worker, 'own_cuda_libraries', return_value=False):
                    worker.speech_session(FakeOrt(), rvc / 'model.onnx', 'gpu')
        self.assertEqual(calls, [{}, {'directory': str(rvc)}])  # its own wheels first; RVC's only without them

    def test_fit_preserves_short_speech_and_gaps(self):
        result = worker.plan_timing([{"start": 1, "end": 5, "text": "Short."}, {"start": 8, "end": 10, "text": "Later."}], [2, 2], True)
        self.assertEqual([r["start"] for r in result], [1, 8])
        self.assertEqual([r["speed"] for r in result], [1, 1])
        self.assertFalse(any(r["overrun"] or r["shifted"] for r in result))

    def test_fit_never_extremely_compresses_or_drops_words(self):
        result = worker.plan_timing([{"start": 0, "end": 1, "text": "Long"}, {"start": 2, "end": 4, "text": "Next"}], [10, 2], True)
        self.assertEqual(result[0]["speed"], 1.35)
        self.assertTrue(result[0]["overrun"])
        self.assertTrue(result[1]["shifted"])
        self.assertAlmostEqual(result[1]["start"], 10 / 1.35)

    def test_natural_duration_does_not_change_speed(self):
        result = worker.plan_timing([{"start": 0, "end": 2, "text": "Natural"}], [3], False)
        self.assertEqual(result[0]["speed"], 1)
        self.assertEqual(result[0]["length"], 3)

    def test_invalid_timings_rejected(self):
        for duration in (0, -1, float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                worker.plan_timing([{"start": 0, "end": 1, "text": "Test"}], [duration], True)


class AdapterTest(unittest.TestCase):
    def setUp(self):
        test_root = Path(__file__).resolve().parents[1] / "build/voice-tests"
        test_root.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="mvp voice test ", dir=test_root)
        self.folder = Path(self.temp.name)
        (self.folder / "infer").mkdir()
        (self.folder / "voice model.pth").write_bytes(b"CLI fixture only")
        (self.folder / "voice.index").write_bytes(b"CLI fixture only")
        (self.folder / "infer/cli.py").write_text(
            "import sys, json, shutil\nfrom pathlib import Path\na=sys.argv[1:]\n"
            "Path('arguments.json').write_text(json.dumps(a))\n"
            "shutil.copyfile(a[a.index('--input')+1], a[a.index('--output')+1])\n", encoding="utf-8")
        self.config = {"root": str(self.folder), "python": sys.executable, "model": str(self.folder / "voice model.pth"),
                       "index": str(self.folder / "voice.index"), "index_rate": 0.75, "pitch": -3, "protect": 0.33, "speaker_id": 2}
        self.source = self.folder / "source audio.wav"
        with wave.open(str(self.source), "wb") as wav:
            wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(24000); wav.writeframes(b"\0\0" * 24000)

    def tearDown(self):
        self.temp.cleanup()

    def test_cli_arguments_and_paths_with_spaces(self):
        target = self.folder / "converted voice.wav"
        worker.convert_rvc(self.config, self.source, target, self.folder)
        self.assertEqual(target.read_bytes(), self.source.read_bytes())
        args = json.loads((self.folder / "arguments.json").read_text())
        for key, value in (("--pitch", "-3"), ("--index-rate", "0.75"), ("--protect", "0.33"), ("--speaker-id", "2"), ("--f0-method", "rmvpe")):
            self.assertEqual(args[args.index(key) + 1], value)
        self.assertEqual(args[args.index("--model") + 1], str(self.folder / "voice model.pth"))

    def test_missing_index_rejected_before_running(self):
        self.config["index"] = ""
        with self.assertRaises(ValueError):
            worker.rvc_command(self.config, self.source, self.folder / "out.wav")
        self.config["index_rate"] = 0
        args, _ = worker.rvc_command(self.config, self.source, self.folder / "out.wav")
        self.assertNotIn("--index", args)

    def test_cli_failure_is_reported(self):
        (self.folder / "infer/cli.py").write_text("raise RuntimeError('Broken RVC environment')")
        with self.assertRaisesRegex(RuntimeError, "Broken RVC environment"):
            worker.convert_rvc(self.config, self.source, self.folder / "out.wav", self.folder)

    def test_cancelled_conversion_stops_child(self):
        (self.folder / "infer/cli.py").write_text("import time\ntime.sleep(30)")
        (self.folder / "cancel").touch()
        with self.assertRaisesRegex(RuntimeError, "Cancelled"):
            worker.convert_rvc(self.config, self.source, self.folder / "out.wav", self.folder)

    def test_assets_check_hash_not_just_size(self):
        file = self.folder / "asset.bin"
        file.write_bytes(b"good")
        asset = {"size": 4, "sha256": hashlib.sha256(b"good").hexdigest()}
        worker.verify(file, asset)
        file.write_bytes(b"evil")
        with self.assertRaisesRegex(ValueError, "checksum"):
            worker.verify(file, asset)

    def test_bundled_batch_uses_one_child_and_preserves_paths(self):
        target = self.folder / "converted voice.wav"
        second = self.folder / "second phrase.wav"
        fixture = self.folder / "bundled.py"
        fixture.write_text("import sys,json,shutil\nfrom pathlib import Path\n"
                           "r=json.loads(Path(sys.argv[1]).read_text())\n"
                           "for i in r['inputs']: shutil.copyfile(i['input'],i['output'])\n"
                           "print(json.dumps({'progress':1,'message':'Converted'}),flush=True)\n")
        real_popen = worker.subprocess.Popen
        calls = []
        def launch(args, **kwargs):
            calls.append(args)
            return real_popen([sys.executable, str(fixture), args[1]], **kwargs)
        inputs = [{"input": str(self.source), "output": str(target)}, {"input": str(self.source), "output": str(second)}]
        with patch.object(worker.subprocess, "Popen", side_effect=launch), patch.object(worker, "emit") as emit:
            worker.convert_bundled_rvc({"worker": "bundled.exe", "model": str(self.folder / "voice model.pth")}, inputs, self.folder)
        self.assertEqual(len(calls), 1)
        self.assertEqual(target.read_bytes(), self.source.read_bytes())
        self.assertEqual(second.read_bytes(), self.source.read_bytes())
        self.assertEqual(json.loads((self.folder / "rvc-request.json").read_text())["inputs"], inputs)

    def test_bundled_failure_reports_engine_error(self):
        fixture = self.folder / "broken.py"
        fixture.write_text("import json,sys\nprint(json.dumps({'error':'Unsupported RVC voice'}),flush=True)\nsys.exit(1)")
        real_popen = worker.subprocess.Popen
        with patch.object(worker.subprocess, "Popen", side_effect=lambda args, **kw: real_popen([sys.executable, str(fixture), args[1]], **kw)):
            with self.assertRaisesRegex(RuntimeError, "Unsupported RVC voice"):
                worker.convert_bundled_rvc({"worker": "bundled.exe"}, [{"input": str(self.source), "output": str(self.folder / "out.wav")}], self.folder)

    def test_bundled_preflight_rejects_missing_engine(self):
        with self.assertRaisesRegex(ValueError, "engine is missing"):
            worker.bundled_rvc_preflight({"worker": str(self.folder / "missing.exe")})

    def test_bundled_cancel_stops_child(self):
        fixture = self.folder / "sleep.py"
        fixture.write_text("import time\ntime.sleep(30)")
        real_popen = worker.subprocess.Popen
        child = []
        def launch(args, **kw):
            child.append(real_popen([sys.executable, str(fixture)], **kw))
            return child[-1]
        (self.folder / "cancel").touch()
        with patch.object(worker.subprocess, "Popen", side_effect=launch):
            with self.assertRaisesRegex(RuntimeError, "Cancelled"):
                worker.convert_bundled_rvc({"worker": "bundled.exe"}, [{"input": str(self.source), "output": str(self.folder / "out.wav")}], self.folder)
        self.assertIsNotNone(child[0].poll())


if __name__ == "__main__":
    unittest.main()
