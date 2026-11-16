import json
import subprocess
import sys
import urllib.request


def token():
    proc = subprocess.run(['git', 'credential', 'fill'],
                          input='protocol=https\nhost=github.com\n\n',
                          capture_output=True, text=True)
    for line in proc.stdout.splitlines():
        if line.startswith('password='):
            return line.split('=', 1)[1]
    raise SystemExit('no credential')


tok = token()
branch = 'fix/lsdriver-cfi-port-20261112'
hdr = {'Authorization': 'Bearer ' + tok,
       'Accept': 'application/vnd.github+json',
       'Content-Type': 'application/json'}

body = json.dumps({'ref': branch, 'inputs': {'kmi': 'android15-6.6'}}).encode()
req = urllib.request.Request(
    'https://api.github.com/repos/libUE4/nbdriver/actions/workflows/kernel-matrix.yml/dispatches',
    data=body, headers=hdr, method='POST')
try:
    with urllib.request.urlopen(req, timeout=60) as resp:
        print('dispatch status', resp.status)
except urllib.error.HTTPError as exc:
    print('dispatch failed', exc.code, exc.read().decode()[:400])
    sys.exit(1)
