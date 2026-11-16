import hashlib
import io
import json
import os
import subprocess
import urllib.request
import zipfile


def token():
    proc = subprocess.run(['git', 'credential', 'fill'],
                          input='protocol=https\nhost=github.com\n\n',
                          capture_output=True, text=True)
    for line in proc.stdout.splitlines():
        if line.startswith('password='):
            return line.split('=', 1)[1]
    raise SystemExit('no credential')


class NoRedirectAuth(urllib.request.HTTPRedirectHandler):
    """Follow the S3 redirect without forwarding the GitHub bearer token."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = urllib.request.Request(newurl)
        return new


opener = urllib.request.build_opener(NoRedirectAuth)
tok = token()
art = 14221465575
req = urllib.request.Request(
    f'https://api.github.com/repos/libUE4/nbdriver/actions/artifacts/{art}/zip',
    headers={'Authorization': 'Bearer ' + tok, 'Accept': 'application/vnd.github+json'})
raw = opener.open(req, timeout=120).read()
z = zipfile.ZipFile(io.BytesIO(raw))
os.makedirs('out/ci2', exist_ok=True)
for name in z.namelist():
    z.extract(name, 'out/ci2')
    print('extracted', name)
for root, _, files in os.walk('out/ci2'):
    for f in sorted(files):
        p = os.path.join(root, f)
        digest = hashlib.sha256(open(p, 'rb').read()).hexdigest()
        print(p, os.path.getsize(p), digest)
