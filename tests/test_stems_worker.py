"""Checks of the stem separation worker's request handling and how it cuts long sound into pieces (no Demucs needed)."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("stems_worker", Path(__file__).resolve().parents[1] / "data/stems/worker.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class RequestTest(unittest.TestCase):
    def test_a_request_is_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            wav = Path(folder) / "in.wav"
            wav.write_bytes(b"RIFF")
            good = {"input": str(wav), "output": folder, "stems": 4, "model": "htdemucs_ft", "device": "cuda"}
            self.assertEqual(worker.validate(dict(good)), good)
            for bad in ({"stems": 3}, {"model": "spleeter"}, {"device": "tpu"}, {"output": ""}, {"input": str(wav) + ".missing"}):
                with self.assertRaises(ValueError):
                    worker.validate({**good, **bad})

    def test_stem_names(self):
        self.assertEqual(worker.names(2), ("voice", "background"))
        self.assertEqual(worker.names(4), ("vocals", "drums", "bass", "other"))


class ChunkTest(unittest.TestCase):
    def test_short_sound_is_one_piece(self):
        self.assertEqual(worker.chunks(1000, rate=100), [(0, 1000)])

    def test_long_sound_is_cut_into_overlapping_pieces_that_cover_it(self):
        pieces = worker.chunks(10000, rate=100, seconds=30, overlap=1)
        self.assertEqual(pieces[0][0], 0)
        self.assertEqual(pieces[-1][1], 10000)
        for (a0, b0), (a1, b1) in zip(pieces, pieces[1:]):
            self.assertEqual(b0 - a1, 100)  # (overlapping by a second)

    def test_crossfades_add_up_to_one(self):
        rate, frames = 100, 10000
        pieces = worker.chunks(frames, rate=rate, seconds=30, overlap=1)
        total = [0.0] * frames
        for n, (a, b) in enumerate(pieces):
            for i, w in enumerate(worker.crossfade(b - a, rate, n == 0, n == len(pieces) - 1)):
                total[a + i] += w
        self.assertTrue(all(abs(t - 1.0) < 1e-9 for t in total))


if __name__ == "__main__":
    unittest.main()
