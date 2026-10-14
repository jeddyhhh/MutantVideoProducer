"""Regenerate Windows wheel locks and their catalog checksums using pinned uv."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--uv', type=Path, default=ROOT / 'deps/ai-bootstrap/uv.exe')
    args = parser.parse_args()
    catalog_path = ROOT / 'data/runtime/catalog.json'
    catalog = json.loads(catalog_path.read_text(encoding='utf-8'))
    if hashlib.sha256(args.uv.read_bytes()).hexdigest() != catalog['uv_sha256']:
        raise ValueError('Use the pinned official uv executable from download-ai-bootstrap.ps1')
    for name in ('speech', 'speech-cuda', 'rvc', 'rvc-cuda'):
        command = [str(args.uv.resolve()), 'pip', 'compile', f'data/runtime/{name}.in',
                   '--python-version', '3.12', '--python-platform', 'x86_64-pc-windows-msvc',
                   '--generate-hashes', '--only-binary', ':all:', '--no-header',
                   '--output-file', f'data/runtime/locks/{name}.txt']
        if name == 'speech-cuda':
            command.append('--no-deps')  # overlay replaces CPU ONNX Runtime after sync
        subprocess.run(command, cwd=ROOT, check=True)
    for component in catalog['components']:
        for key in ('lock', 'cuda_lock'):
            path = ROOT / 'data/runtime' / component[key]
            component[key + '_sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
    catalog_path.write_text(json.dumps(catalog, indent=2) + '\n', encoding='utf-8')
