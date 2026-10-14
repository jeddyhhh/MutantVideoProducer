"""Store tested branch builds in a draft release, independently of Actions storage."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

PART_SIZE = 1024 ** 3  # Release assets must be smaller than 2 GiB.


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def parts(path, limit=PART_SIZE, threshold=None):
    """Yield one bounded part at a time; leave the original file untouched."""
    path = Path(path)
    if limit <= 0:
        raise ValueError('Part size must be positive')
    if path.stat().st_size <= (limit if threshold is None else threshold):
        yield path, False
        return
    with path.open('rb') as source:
        index = 1
        while source.tell() < path.stat().st_size:
            part = path.with_name(f'{path.name}.part{index:03d}')
            with part.open('xb') as destination:
                remaining = limit
                while remaining:
                    chunk = source.read(min(remaining, 8 * 1024 * 1024))
                    if not chunk:
                        break
                    destination.write(chunk)
                    remaining -= len(chunk)
            yield part, True
            index += 1


def gh(*args):
    result = subprocess.run(['gh', *args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or 'GitHub CLI request failed')
    return result.stdout


def draft_release(repo, commit):
    tag = f'ci-{commit[:12]}'
    def find():
        releases = json.loads(gh('api', f'repos/{repo}/releases?per_page=100'))
        return next((item for item in releases if item['tag_name'] == tag), None)
    release = find()
    if release is None:
        try:
            # Use the creation response: the release list can lag behind a successful POST.
            release = json.loads(gh('api', '--method', 'POST', f'repos/{repo}/releases',
               '-f', f'tag_name={tag}', '-f', f'target_commitish={commit}',
               '-F', 'draft=true', '-F', 'prerelease=true', '-f', f'name=Development builds {commit[:12]}',
               '-f', f'body=Tested CI downloads for commit {commit}. Windows and Linux workflows attach their files here. '
               'These builds remain drafts; formal releases use version tags. SHA-256 files accompany each download. '
               'The small Windows installer excludes optional Speech/RVC engines. Download AI engines directly from upstream sources through Tools > AI Components; '
               'Python setup is automatic and no AI packs need to be downloaded manually. '
               'If a large file is split into .part001, .part002, etc., download every matching part and its .sha256 file, then run '
               '`python join-build-parts.py <filename.part001>` to reconstruct it.'))
        except RuntimeError:
            # The other platform may have created the same draft concurrently.
            for attempt in range(6):
                release = find()
                if release is not None:
                    break
                time.sleep(2)
            if release is None:
                raise
    if release is None or not release['draft']:
        raise RuntimeError('Refusing to attach development builds to a published release')
    return tag, release['html_url']


def upload(repo, tag, path):
    for attempt in range(3):
        try:
            gh('release', 'upload', tag, str(path), '--repo', repo, '--clobber')
            print(f'Uploaded {path.name}', flush=True)
            return
        except RuntimeError:
            if attempt == 2:
                raise
            time.sleep(10 * (attempt + 1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='+', type=Path)
    args = parser.parse_args()
    repo, commit = os.environ.get('GITHUB_REPOSITORY', ''), os.environ.get('GITHUB_SHA', '')
    if not re.fullmatch(r'[\w.-]+/[\w.-]+', repo) or not re.fullmatch(r'(?:[0-9a-f]{40}|[0-9a-f]{64})', commit):
        raise ValueError('Expected a GitHub Actions repository and commit SHA')
    if not os.environ.get('GH_TOKEN') or not shutil.which('gh'):
        raise RuntimeError('GitHub CLI and GH_TOKEN are required')
    for path in args.files:
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f'Build file is missing or empty: {path}')
    tag, url = draft_release(repo, commit)
    multipart = False
    for path in args.files:
        checksum = path.with_name(path.name + '.sha256')
        checksum.write_text(f'{sha256(path)} *{path.name}\n', encoding='ascii')
        for part, temporary in parts(path, threshold=2 * 1024 ** 3 - 1):
            upload(repo, tag, part)
            if temporary:
                multipart = True
                part.unlink()
        upload(repo, tag, checksum)
    if multipart:
        upload(repo, tag, Path(__file__).with_name('join-build-parts.py'))
    print(f'Build downloads: {url}', flush=True)
    summary = os.environ.get('GITHUB_STEP_SUMMARY')
    if summary:
        with open(summary, 'a', encoding='utf-8') as stream:
            stream.write(f'### Build downloads\n\n[Draft release with tested builds]({url})\n')


if __name__ == '__main__':
    main()
