import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().parents[1] / 'scripts' / file)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


store = module('ci_store', 'store-ci-build.py')
reassemble = module('ci_join', 'join-build-parts.py')


class CiDownloadTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.file = Path(self.temp.name) / 'build.tar.gz'
        self.content = bytes(range(256)) * 17
        self.file.write_bytes(self.content)
        self.file.with_name(self.file.name + '.sha256').write_text(
            f'{hashlib.sha256(self.content).hexdigest()} *{self.file.name}\n', encoding='ascii')

    def split(self):
        return [part for part, temporary in store.parts(self.file, 1000)]

    def test_round_trip_and_bounded_parts(self):
        parts = self.split()
        self.assertEqual(len(parts), 5)
        self.assertTrue(all(part.stat().st_size <= 1000 for part in parts))
        self.file.unlink()
        self.assertEqual(reassemble.join(parts[0]).read_bytes(), self.content)

    def test_missing_part_does_not_create_output(self):
        parts = self.split()
        self.file.unlink()
        parts[1].unlink()
        with self.assertRaises(ValueError):
            reassemble.join(parts[0])
        self.assertFalse(self.file.exists())

    def test_corrupt_or_truncated_download_is_rejected(self):
        parts = self.split()
        self.file.unlink()
        parts[-1].write_bytes(b'corrupt')
        with self.assertRaises(ValueError):
            reassemble.join(parts[0])
        self.assertFalse(self.file.exists())

    def test_existing_file_is_preserved(self):
        parts = self.split()
        with self.assertRaises(FileExistsError):
            reassemble.join(parts[0])
        self.assertEqual(self.file.read_bytes(), self.content)

    def test_small_download_and_digest(self):
        self.assertEqual(list(store.parts(self.file, 5000)), [(self.file, False)])
        self.assertEqual(store.sha256(self.file), hashlib.sha256(self.content).hexdigest())

    def test_file_below_release_limit_is_not_split_unnecessarily(self):
        self.assertEqual(list(store.parts(self.file, 1000, threshold=5000)), [(self.file, False)])


if __name__ == '__main__':
    unittest.main()
