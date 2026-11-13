#!/usr/bin/env python3
"""Fail if FD task_work can return to module text after its last module_put.

Inspect the actual AArch64 module, including LTO builds; source spelling or a
compiler's optional sibling-call optimization is not sufficient evidence.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--module',required=True,type=Path)
parser.add_argument('--objdump')
a=parser.parse_args()
objdump=a.objdump or next((p for n in ('llvm-objdump','llvm-objdump-18','aarch64-linux-gnu-objdump') if (p:=shutil.which(n))),None)
if not objdump:
    raise SystemExit('AArch64 objdump unavailable')
r=subprocess.run([objdump,'-dr',str(a.module)],capture_output=True,text=True,check=True)
functions=re.split(r'(?m)^([0-9a-f]+) <([^>]+)>:\n',r.stdout)
for i in range(1,len(functions),3):
    name=functions[i+1];body=functions[i+2]
    if not name.startswith('nbdriver_fd_work_fn') or not re.search(r'R_AARCH64_JUMP26\s+module_put\b',body):
        continue
    instructions=[]
    for line in body.splitlines():
        m=re.match(r'\s*[0-9a-f]+:\s+[0-9a-f]{8}\s+([a-z][a-z0-9.]*)\s*(.*)',line)
        if m:
            instructions.append((m.group(1),m.group(2)))
    # The first four instructions preserve the call boundary. The last four
    # restore it and branch to core kernel code; no epilogue remains in nbdriver.
    expected=['bti','paciasp','stp','mov','bl','ldp','autiasp','b']
    if [x[0] for x in instructions] != expected:
        raise SystemExit('unexpected callback machine code: '+str(instructions))
    if not re.search(r'R_AARCH64_CALL26\s+nbdriver_fd_work_run\b',body):
        raise SystemExit('callback does not call its C worker')
    if not all(reg in instructions[2][1] and reg in instructions[5][1] for reg in ('x29','x30','sp')):
        raise SystemExit('callback does not restore its caller frame before module_put')
    print(body.strip())
    print('PASS: actual callback restores LR/SP, then jumps to core module_put; no module return instructions remain')
    break
else:
    raise SystemExit('no verified FD callback tail transfer in '+str(a.module))
