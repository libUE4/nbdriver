#!/usr/bin/env python3
"""Exercise production auto devpts code through explicit kernel adapters."""
from pathlib import Path
import os,re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/nbdriver_features.c').read_text()
s=s[s.index('/* ===== BEGIN src/features/nbdriver_devpts.c ===== */'):]
s=re.sub(r'^#include[^\n]*\n','',s,flags=re.M)
with tempfile.TemporaryDirectory(prefix='nbdriver-auto-') as name:
    temp=Path(name)
    (temp/'devpts_production.c').write_text(s)
    binary=temp/'probe'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-I'+str(temp),str(root/'tests/devpts_auto_host_check.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
