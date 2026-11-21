#!/usr/bin/env python3
"""Run the production frame router with UBSan/bounds; no Android/device claim.

ASan's reserved virtual address space is unavailable in this PRoot environment.
"""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nbdriver-touch-route-') as folder:
    exe = pathlib.Path(folder) / 'test'
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined,bounds', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                    '-I' + str(root / 'sdk/include'), '-I' + str(root / 'src'),
                    str(root / 'tests/touch_route_test.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
