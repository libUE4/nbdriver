#!/usr/bin/env python3
"""Execute production KGSL code with host adapters, then C/C++ SDK probes."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/nbdriver_features.c').read_text()
with tempfile.TemporaryDirectory(prefix='nbdriver-kgsl-') as d:
 p=Path(d)
 for section,fixture in [('nbdriver_kgsl','kgsl_host_check.c'),('nbdriver_gpu_visibility','gpu_visibility_host_check.c')]:
  if not (root/'tests'/fixture).exists():continue
  code=s.split('/* ===== BEGIN src/features/'+section+'.c ===== */',1)[1].split('/* ===== END src/features/'+section+'.c ===== */',1)[0]
  code=re.sub(r'^#include[^\n]*\n','',code,flags=re.M)
  if section=='nbdriver_kgsl':
   # The production feature table now advertises the viewer-scoped proc
   # backend, but this isolated KGSL fixture intentionally excludes devpts.
   code=('static int nbdriver_proc_visibility_init(void){return 0;}\n'
         'static void nbdriver_proc_visibility_exit(void){}\n'
         'static u32 nbdriver_proc_visibility_features(void){return 0;}\n'+code)
  name='kgsl_production.inc' if section=='nbdriver_kgsl' else 'gpu_visibility_production.inc'
  (p/name).write_text(code)
  binary=p/section
  subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fno-pie','-no-pie','-fsanitize=undefined,bounds','-I'+str(root/'include'),'-I'+str(p),str(root/'tests'/fixture),'-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
