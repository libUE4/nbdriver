#!/usr/bin/env python3
"""Compile the production trampoline and test encoded tail-transfer validation.

Formatting adapters cover word/byte output and hint aliases. Unsafe frame,
worker and return paths are assembled separately and must remain rejected.
"""
from pathlib import Path
import ast
import os
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
clang = os.environ.get('CLANG') or shutil.which('clang') or shutil.which('clang-18')
objdump = os.environ.get('LLVM_OBJDUMP') or shutil.which('llvm-objdump') or shutil.which('llvm-objdump-18')
if not objdump:
    objdump = next((str(p) for p in sorted(Path('/usr/bin').glob('llvm-objdump-[0-9]*'), reverse=True)), None)
assert clang and objdump, 'Clang and an LLVM AArch64 objdump are required'
source = (root / 'src/nbdriver_platform.c').read_text()
start = source.index('static __naked void nbdriver_fd_work_fn(')
end = source.index('static int nbdriver_schedule_fd(', start)
assembly = ''.join(ast.literal_eval('"' + value + '"') for value in re.findall(r'"((?:\\.|[^"\\])*)"', source[start:end]))

with tempfile.TemporaryDirectory(prefix='nbdriver-callback-') as directory:
    temp = Path(directory)
    def compile_code(name, code):
        path = temp / (name + '.S')
        path.write_text('.text\n.p2align 2\n.global nbdriver_fd_work_fn\n.type nbdriver_fd_work_fn,%function\nnbdriver_fd_work_fn:\n' + code + '\n.size nbdriver_fd_work_fn,.-nbdriver_fd_work_fn\n')
        output = temp / (name + '.o')
        subprocess.run([clang, '--target=aarch64-linux-gnu', '-c', str(path), '-o', str(output)], check=True)
        return output
    def check(module, tool, accepted):
        result = subprocess.run(['python3', str(root / 'tools/check_callback_tail.py'), '--module', str(module), '--objdump', str(tool)], capture_output=True, text=True)
        assert (result.returncode == 0) == accepted, result.stdout + result.stderr
    module = compile_code('production', assembly)
    check(module, objdump, True)
    check(compile_code('compiler-bti-entry', '.inst 0xd503245f\n' + assembly), objdump, True)
    disassembly = subprocess.check_output([objdump, '-dr', str(module)], text=True)
    for style in ('words', 'bytes', 'aliases'):
        lines = []
        for line in disassembly.splitlines():
            match = re.match(r'^(\s*[0-9a-f]+:\s+)([0-9a-f]{8}|[0-9a-f]{2}(?:[ \t]+[0-9a-f]{2}){3})(\s+.+)$', line, re.I)
            if match:
                raw = match.group(2)
                word = int.from_bytes(bytes.fromhex(raw), 'little') if ' ' in raw or '\t' in raw else int(raw, 16)
                encoded = ' '.join(f'{v:02x}' for v in word.to_bytes(4, 'little')) if style == 'bytes' else f'{word:08x}'
                line = match.group(1) + encoded + match.group(3)
                if style == 'aliases':
                    line = re.sub(r'\bbti\s+c\b', 'hint #34', line).replace('paciasp', 'hint #25').replace('autiasp', 'hint #29')
            lines.append(line)
        adapter = temp / ('objdump-' + style)
        adapter.write_text('#!/usr/bin/env python3\nimport sys\nsys.stdout.write(' + repr('\n'.join(lines) + '\n') + ')\n')
        adapter.chmod(0o755)
        check(module, adapter, True)
    unsafe = [
        ('unexpected-entry', 'nop\n' + assembly),
        ('wrong-frame', assembly.replace('ldp x29, x30', 'ldp x19, x20')),
        ('return-to-module', assembly.replace('b module_put', 'bl module_put')),
        ('wrong-worker', assembly.replace('bl nbdriver_fd_work_run', 'bl different_worker')),
    ]
    for name, code in unsafe:
        assert code != assembly
        check(compile_code(name, code), objdump, False)
print('PASS callback: real production assembly, word/byte/alias formats, unsafe frame/worker/return paths rejected')
