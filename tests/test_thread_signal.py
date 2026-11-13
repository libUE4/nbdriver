#!/usr/bin/env python3
"""Exercise actual read-only signal ioctl body with mocked kernel locks/identity."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/nbdriver_core.c').read_text()
a=s.index('static long query_signal(unsigned long argument)');z=s.index('\nlong nbdriver_query_ioctl',a)
with tempfile.TemporaryDirectory(prefix='nbdriver-signal-') as d:
 p=Path(d);(p/'signal_query.inc').write_text(s[a:z]);binary=p/'check'
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-O2','-pthread','-I'+str(root/'include'),'-I'+d,str(root/'tests/thread_signal_host_check.c'),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
