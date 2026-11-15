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
functions=re.split(r'(?mi)^([0-9a-f]+) <([^>]+)>:\n',r.stdout)
for i in range(1,len(functions),3):
    name=functions[i+1];body=functions[i+2]
    if not name.startswith('nbdriver_fd_work_fn') or not re.search(r'R_AARCH64_JUMP26\s+module_put\b',body):
        continue
    instructions=[]
    for line in body.splitlines():
        # LLVM versions print either a word or four little-endian bytes.
        # Compare encoded instructions so hint/BTI/PAC aliases cannot affect
        # validation, and still require exact frame restoration and tail jump.
        m=re.match(r'\s*([0-9a-f]+):\s+([0-9a-f]{8}|[0-9a-f]{2}(?:[ \t]+[0-9a-f]{2}){3})\s+(.+)',line,re.I)
        if m:
            raw=m.group(2)
            word=int.from_bytes(bytes.fromhex(raw),'little') if re.search(r'\s',raw) else int(raw,16)
            instructions.append((int(m.group(1),16),word))
    # Newer toolchains may add one BTI landing pad before the explicit BTI.
    # This does not change LR/SP; do not allow any other extra instructions.
    if len(instructions)==9 and instructions[0][1]==instructions[1][1]==0xd503245f:
        instructions=instructions[1:]
    # The first four instructions preserve the call boundary. The last four
    # restore it and branch to core kernel code; no epilogue remains in nbdriver.
    expected=[0xd503245f,0xd503233f,0xa9bf7bfd,0x910003fd,
              0x94000000,0xa8c17bfd,0xd50323bf,0x14000000]
    words=[word & 0xfc000000 if index in (4,7) else word
           for index,(_,word) in enumerate(instructions)]
    if words != expected:
        print(body.strip())
        raise SystemExit('unexpected callback machine code: '+str(instructions))
    def relocation(index,kind,symbol):
        return re.search(r'(?mi)^\s*0*'+format(instructions[index][0],'x')+
                         r':\s+'+kind+r'\s+'+symbol+r'\b',body)
    if not relocation(4,'R_AARCH64_CALL26','nbdriver_fd_work_run'):
        raise SystemExit('callback does not call its C worker')
    if not relocation(7,'R_AARCH64_JUMP26','module_put'):
        raise SystemExit('callback does not tail-jump to core module_put')
    print(body.strip())
    print('PASS: actual callback restores LR/SP, then jumps to core module_put; no module return instructions remain')
    break
else:
    raise SystemExit('no verified FD callback tail transfer in '+str(a.module))
