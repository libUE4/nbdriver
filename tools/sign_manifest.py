#!/usr/bin/env python3
"""Sign a verified module publication using an offline/root-owned Ed25519 key.

The private key stays on the publication host. Clients embed only the public key.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

DOMAIN = b'NBG-NBDRIVER-MANIFEST-v1\n'

def sign(directory, key):
    path = directory / 'nbdriver-manifest.json'
    body = path.read_bytes()
    manifest = json.loads(body)
    if manifest.get('version') != 2 or not 1 <= len(manifest['files']) <= 256:
        raise ValueError('verified version 2 publication required')
    for row in manifest['files']:
        name = row['name']
        if not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]*\.ko', name) or '..' in name:
            raise ValueError('unsafe module name')
        module = directory / name
        if module.is_symlink() or not module.is_file():
            raise ValueError('regular module required')
        data = module.read_bytes()
        if len(data) != row['size'] or hashlib.sha256(data).hexdigest() != row['sha256']:
            raise ValueError('module differs from publication: ' + name)
    digest = hashlib.sha256(body).hexdigest()
    sidecar = directory / 'nbdriver-manifest.signature.json'
    previous = json.loads(sidecar.read_bytes()) if sidecar.exists() else {}
    # A re-publication gets a greater serial even if the wall clock moves back.
    sequence = max(time.time_ns() // 1000000, int(previous.get('sequence', 0)) + 1)
    payload = DOMAIN + str(sequence).encode('ascii') + b'\n' + body
    with tempfile.TemporaryDirectory(prefix='nbdriver-sign-') as folder:
        folder = Path(folder)
        (folder / 'message').write_bytes(payload)
        subprocess.run(['openssl', 'pkeyutl', '-sign', '-rawin', '-inkey', str(key),
                        '-in', str(folder / 'message'), '-out', str(folder / 'signature')], check=True)
        signature = (folder / 'signature').read_bytes()
        if len(signature) != 64:
            raise ValueError('Ed25519 signature size')
    result = dict(version=1, algorithm='ed25519', sequence=sequence,
                  manifest_sha256=digest, signature=signature.hex())
    data = (json.dumps(result, sort_keys=True, separators=(',', ':')) + '\n').encode()
    fd, name = tempfile.mkstemp(prefix='.manifest-signature-', dir=directory)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data); stream.flush(); os.fsync(stream.fileno())
        os.chmod(name, 0o644)
        os.replace(name, sidecar)
    finally:
        Path(name).unlink(missing_ok=True)
    return result

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', required=True, type=Path)
    parser.add_argument('--key', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(sign(args.directory, args.key), sort_keys=True))
