"""Prepare the standalone Windows installer payload with upstream AI setup."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile
import zipfile

GIB = 1024 ** 3
OPTIONAL = ('voice/worker', 'voice/rvc-worker', 'voice/assets', 'models/whisper')


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def archive(source, target):
    with zipfile.ZipFile(target, 'x', zipfile.ZIP_DEFLATED, compresslevel=3, allowZip64=True) as output:
        for path in sorted(source.rglob('*')):
            if path.is_symlink():
                raise ValueError(f'Unexpected symbolic link in runtime: {path}')
            if path.is_file():
                output.write(path, path.relative_to(source).as_posix())


def split_archive(path, limit=GIB, threshold=2 * GIB - 1):
    if path.stat().st_size <= threshold:
        return [path]
    result = []
    with path.open('rb') as source:
        while source.tell() < path.stat().st_size:
            part = path.with_name(f'{path.name}.part{len(result)+1:03d}')
            with part.open('xb') as output:
                remaining = limit
                while remaining:
                    chunk = source.read(min(8 * 1024 * 1024, remaining))
                    if not chunk:
                        break
                    output.write(chunk)
                    remaining -= len(chunk)
            result.append(part)
    path.unlink()  # Only our newly created archive; its published parts replace it.
    return result


def base_file(path):
    relative = path.as_posix()
    if '__pycache__' in path.parts or relative == 'runtime/extract-runtime.ps1':
        return False
    if any(relative == prefix or relative.startswith(prefix + '/') for prefix in OPTIONAL):
        return False
    name = path.name.lower()
    return not (name.endswith('.pdb') or name == 'qoffscreen.dll' or name.startswith('montage')
                or (name.startswith('mvp_') and name.endswith('.exe')))


def package(bin_dir, output, version):
    bin_dir, output = Path(bin_dir).resolve(), Path(output).resolve()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]{0,60}', version):
        raise ValueError('Invalid distribution version')
    if output.exists() or output == bin_dir or bin_dir in output.parents or output in bin_dir.parents:
        raise ValueError('Use a new output folder outside the application runtime')
    if not (bin_dir / 'MutantVideoProducer.exe').is_file():
        raise ValueError('The compiled application is missing')
    output.parent.mkdir(parents=True, exist_ok=True)
    prefix = f'MutantVideoProducer-{version}-win64'
    published = []
    with tempfile.TemporaryDirectory(prefix='windows-package-', dir=output.parent) as temporary:
        stage = Path(temporary)
        base = stage / prefix
        base.mkdir()
        for path in bin_dir.rglob('*'):
            relative = path.relative_to(bin_dir)
            if path.is_file() and base_file(relative):
                if path.is_symlink():
                    raise ValueError(f'Unexpected symbolic link in application: {path}')
                target = base / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
        for name in ('runtime/uv.exe', 'runtime/catalog.json', 'voice/worker.py', 'voice/rvc/worker.py', 'stems/worker.py'):
            if not (base / name).is_file():
                raise ValueError(f'The upstream AI setup file is missing: {name}')
        catalog = json.loads((base / 'runtime/catalog.json').read_text(encoding='utf-8'))
        if sha256(base / 'runtime/uv.exe') != catalog['uv_sha256']:
            raise ValueError('The AI bootstrap checksum does not match')
        for component in catalog['components']:
            for key in ('lock', 'cuda_lock'):
                if sha256(base / 'runtime' / component[key]) != component[key + '_sha256']:
                    raise ValueError('An AI dependency lock checksum does not match')
        (base / 'INSTALL-NOTES.txt').write_text(
            'Mutant Video Producer installs as one normal Windows program.\n'
            'Optional Speech Generation and RVC engines download from upstream sources in Tools > AI Components.\n'
            'Python setup is automatic; no separate installer is needed.\n'
            'Voice/model downloads remain available through their existing libraries.\n', encoding='utf-8')
        packed_base = stage / f'{prefix}.zip'
        archive(base, packed_base)
        if packed_base.stat().st_size >= GIB:
            raise ValueError('The base ZIP exceeds the 1 GiB packaging budget')
        published.append(packed_base.name)
        symbols = stage / f'{prefix}-symbols.zip'
        with zipfile.ZipFile(symbols, 'x', zipfile.ZIP_DEFLATED, compresslevel=3) as packed:
            for path in bin_dir.glob('*.pdb'):
                packed.write(path, path.name)
        published.append(symbols.name)
        for name in published:
            path = stage / name
            path.with_name(name + '.sha256').write_text(f'{sha256(path)} *{name}\n', encoding='ascii')
        (stage / 'files.json').write_text(json.dumps(published), encoding='utf-8')
        stage.rename(output)
    print(f'Base ZIP: {(output / packed_base.name).stat().st_size / 1000000:.1f} MB', flush=True)
    return output / prefix


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    package(args.bin, args.output, args.version)
