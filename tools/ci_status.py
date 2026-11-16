import json
import subprocess
import sys
import urllib.request

proc = subprocess.run(
    ['git', 'credential', 'fill'],
    input='protocol=https\nhost=github.com\n\n',
    capture_output=True, text=True)
password = ''
for line in proc.stdout.splitlines():
    if line.startswith('password='):
        password = line.split('=', 1)[1]
if not password:
    sys.exit('no credential')

req = urllib.request.Request(
    'https://api.github.com/repos/libUE4/nbdriver/actions/runs?per_page=5',
    headers={'Authorization': 'Bearer ' + password,
             'Accept': 'application/vnd.github+json'})
data = json.load(urllib.request.urlopen(req, timeout=60))
for run in data.get('workflow_runs', []):
    print(run['id'], run['head_branch'], run['head_sha'][:8],
          run['status'], run['conclusion'], run['created_at'])
    print('   ', run['html_url'])
