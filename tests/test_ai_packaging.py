import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('ai_packaging', Path(__file__).resolve().parents[1] / 'scripts/package-windows.py')
packaging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packaging)


class AiPackagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / 'bin'
        for name in ('MutantVideoProducer.exe', 'Qt6Core.dll', 'runtime/uv.exe', 'voice/worker.py', 'voice/rvc/worker.py', 'stems/worker.py',
                     'voice/worker/mvp-voice-worker.exe', 'voice/worker/_internal/gpu.dll',
                     'voice/rvc-worker/mvp-rvc-worker.exe', 'voice/rvc-worker/_internal/torch.dll',
                     'voice/assets/model.onnx', 'models/whisper/ggml-base.bin', 'voice/catalog.json',
                     'models/segmentation/catalog.json', 'mvp_selftest.exe', 'mvp_runtime_test.exe', 'MutantVideoProducer.pdb'):
            target = self.bin / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(('fixture ' + name).encode())

        catalog = {'version': 2, 'platform': 'windows-x86_64',
                   'uv_sha256': packaging.sha256(self.bin / 'runtime/uv.exe'), 'components': []}
        for id in ('speech', 'rvc'):
            item = {'id': id}
            for key, suffix in (('lock', ''), ('cuda_lock', '-cuda')):
                name = f'locks/{id}{suffix}.txt'
                target = self.bin / 'runtime' / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('fixture locked dependencies')
                item[key] = name
                item[key + '_sha256'] = packaging.sha256(target)
            catalog['components'].append(item)
        (self.bin / 'runtime/catalog.json').write_text(json.dumps(catalog))

    def run_package(self):
        return packaging.package(self.bin, self.root / 'distribution', 'dev-test')

    def test_base_retains_editor_and_catalogs_without_ai_engines_models_or_tests(self):
        base = self.run_package()
        self.assertTrue((base / 'MutantVideoProducer.exe').is_file())
        self.assertTrue((base / 'Qt6Core.dll').is_file())
        self.assertTrue((base / 'voice/catalog.json').is_file())
        self.assertTrue((base / 'models/segmentation/catalog.json').is_file())
        for name in packaging.OPTIONAL:
            self.assertFalse((base / name).exists(), name)
        self.assertFalse((base / 'mvp_runtime_test.exe').exists())
        self.assertFalse((base / 'MutantVideoProducer.pdb').exists())

    def test_bootstrap_and_locks_ship_without_our_own_ai_archives(self):
        base = self.run_package()
        self.assertTrue((base / 'runtime/uv.exe').is_file())
        self.assertTrue((base / 'runtime/locks/speech.txt').is_file())
        self.assertFalse(list(base.parent.glob('*-ai-*')))
        self.assertNotIn('Install Downloaded Pack', (base / 'INSTALL-NOTES.txt').read_text())

    def test_no_frozen_workers_are_required_for_standard_installer(self):
        import shutil
        shutil.rmtree(self.bin / 'voice/worker')
        shutil.rmtree(self.bin / 'voice/rvc-worker')
        self.assertTrue(self.run_package().is_dir())

    def test_damaged_setup_helper_never_publishes_partial_distribution(self):
        (self.bin / 'runtime/uv.exe').write_bytes(b'tampered')
        with self.assertRaises(ValueError):
            self.run_package()
        self.assertFalse((self.root / 'distribution').exists())
        self.assertTrue((self.bin / 'MutantVideoProducer.exe').exists())

    def test_existing_distribution_is_not_overwritten(self):
        base = self.run_package()
        before = (base / 'MutantVideoProducer.exe').read_bytes()
        with self.assertRaises(ValueError):
            self.run_package()
        self.assertEqual((base / 'MutantVideoProducer.exe').read_bytes(), before)

    def test_split_archive_reassembles_exactly(self):
        path = self.root / 'large.zip'
        content = bytes(range(256)) * 21
        path.write_bytes(content)
        parts = packaging.split_archive(path, limit=1000, threshold=2000)
        self.assertFalse(path.exists())
        self.assertTrue(all(part.stat().st_size <= 1000 for part in parts))
        self.assertEqual(b''.join(part.read_bytes() for part in parts), content)


if __name__ == '__main__':
    unittest.main()
