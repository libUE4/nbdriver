#!/usr/bin/env python3
"""Run the actual UDP backend with host kernel adapters; no live app traffic."""
from pathlib import Path
import os,re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/nbdriver_udp.c').read_text()
source=re.sub(r'^#include[^\n]*\n','',source,flags=re.M)
with tempfile.TemporaryDirectory(prefix='nbdriver-udp-')as directory:
    path=Path(directory);(path/'udp_production.inc').write_text(source)
    command=[os.environ.get('CC','cc'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-pthread','-fsanitize=undefined,bounds','-I'+str(root/'include'),'-I'+str(path),str(root/'tests/udp_capture_host_check.c'),'-o',str(path/'check')]
    subprocess.run(command,check=True)
    subprocess.run([str(path/'check')],check=True)
