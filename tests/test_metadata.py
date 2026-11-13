#!/usr/bin/env python3
"""Run actual metadata code with synthetic kernel/VFS/vendor dependencies."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root/'src/nbdriver_features.c').read_text()
source = source.split('/* ===== BEGIN src/features/nbdriver_metadata.c ===== */', 1)[1].split('/* ===== END src/features/nbdriver_metadata.c ===== */', 1)[0]
source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
with tempfile.TemporaryDirectory(prefix='nbdriver-metadata-') as directory:
    temp = Path(directory)
    (temp/'metadata_dispatch.inc').write_text(source)
    for version in (0x050a00, 0x050f00, 0x060600):
        binary = temp/'check'
        cmd = ['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
               '-I'+str(root/'include'),'-I'+str(root/'src'),'-I'+str(temp),
               '-DTEST_VERSION='+str(version), str(root/'tests/metadata_host_check.c'),
               '-o',str(binary)]
        subprocess.run(cmd,check=True)
        subprocess.run([str(binary)],check=True)
