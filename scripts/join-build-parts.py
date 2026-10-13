"""Join a split CI download without loading it into memory or overwriting a file."""
import argparse
import hashlib
from pathlib import Path
import re


def join(first):
    first = Path(first)
    if not first.name.endswith('.part001'):
        raise ValueError('Choose the .part001 file')
    target = first.with_name(first.name[:-8])
    candidates = sorted(first.parent.glob(target.name + '.part*'))
    for index, path in enumerate(candidates, 1):
        if not re.fullmatch(re.escape(target.name) + rf'\.part{index:03d}', path.name):
            raise ValueError('A download part is missing or has an unexpected name')
    if not candidates:
        raise ValueError('No parts were found')
    checksum = target.with_name(target.name + '.sha256').read_text(encoding='ascii').strip()
    match = re.fullmatch(r'([0-9a-f]{64}) \*' + re.escape(target.name), checksum)
    if not match:
        raise ValueError('Download the matching .sha256 file before joining parts')
    digest = hashlib.sha256()
    try:
        with target.open('xb') as output:
            for path in candidates:
                with path.open('rb') as source:
                    for chunk in iter(lambda: source.read(8 * 1024 * 1024), b''):
                        digest.update(chunk)
                        output.write(chunk)
        if digest.hexdigest() != match.group(1):
            raise ValueError('SHA-256 mismatch: a part is missing or corrupted')
    except FileExistsError:
        raise
    except Exception:
        target.unlink(missing_ok=True)
        raise
    return target


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('first_part', type=Path)
    print(join(parser.parse_args().first_part))
