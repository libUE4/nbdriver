#!/usr/bin/env python3
"""Run the real batch-dispatch source with synthetic mm/copy helpers."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/nbdriver_core.c').read_text()
source = source.split('/* ===== BEGIN src/core/nbdriver_read.c ===== */', 1)[1].split('/* ===== END src/core/nbdriver_read.c ===== */', 1)[0]
start = source.index('static int nbdriver_readv_entry_valid(')
with tempfile.TemporaryDirectory(prefix='nbdriver-readv-') as directory:
    temp = Path(directory)
    (temp / 'readv_dispatch.inc').write_text(source[start:])
    command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-I'+str(root/'include'), '-I'+str(temp),
               str(root/'tests/readv_host_check.c'), '-o', str(temp/'check')]
    print('+', ' '.join(command), flush=True)
    subprocess.run(command, check=True)
    subprocess.run([str(temp/'check')], check=True)
