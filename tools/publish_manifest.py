#!/usr/bin/env python3
"""Publish an immutable v2 manifest; distribute its printed digest out of band."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

def publish(directory, profiles, output):
    entries, targets, names = [], set(), set()
    for profile in profiles:
        if set(profile) != {'name', 'kmi', 'arch', 'page_size', 'symvers'}:
            raise ValueError('profile requires name, kmi, arch, page_size, symvers')
        name = profile['name']
        if (not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]*\.ko', name)
                or '..' in name or name in names):
            raise ValueError('invalid or duplicate artifact name')
        kmi = profile['kmi']
        if not isinstance(kmi, str) or not re.fullmatch(r'android\d+-\d+\.\d+', kmi):
            raise ValueError('full Android KMI required')
        if profile['arch'] != 'aarch64' or type(profile['page_size']) is not int or profile['page_size'] not in (4096, 16384, 65536):
            raise ValueError('unsupported architecture or page size')
        target = (kmi, profile['arch'], profile['page_size'])
        if target in targets:
            raise ValueError('ambiguous target profile')
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise ValueError('artifact must be a regular non-symlink file')
        image = path.read_bytes()
        if (not 64 <= len(image) <= 16 * 1024 * 1024 or image[:7] != b'\x7fELF\x02\x01\x01'
                or struct.unpack_from('<HH', image, 16) != (1, 183)):
            raise ValueError('artifact is not an ARM64 relocatable module')
        # This tool does not infer a kernel ABI or page configuration from a
        # filename. Profiles must come from the verified target build records.
        subprocess.run([sys.executable, str(Path(__file__).with_name('check_undefined.py')),
                        '--symvers', profile['symvers'], '--module', str(path), '--kmi', kmi], check=True)
        subprocess.run([sys.executable, str(Path(__file__).with_name('check_callback_tail.py')),
                        '--module', str(path)], check=True)
        published = {key: profile[key] for key in ('name', 'kmi', 'arch', 'page_size')}
        entries.append(dict(published, size=len(image), sha256=hashlib.sha256(image).hexdigest()))
        names.add(name); targets.add(target)
    if not entries:
        raise ValueError('empty release')
    body = (json.dumps({'version': 2, 'files': sorted(entries, key=lambda e: e['name'])},
                       sort_keys=True, separators=(',', ':'), ensure_ascii=True) + '\n').encode()
    # Exclusive creation prevents changing an already pinned publication.
    with output.open('xb') as f:
        f.write(body)
    return hashlib.sha256(body).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--profiles', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    a = parser.parse_args()
    print('NBG_NBDRIVER_MANIFEST_SHA256=' + publish(a.directory, json.loads(a.profiles.read_text()), a.output))

if __name__ == '__main__':
    main()
