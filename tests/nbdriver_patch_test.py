#!/usr/bin/env python3
"""Offline checks against the real patcher and routing implementation.

No module loading, sysctl writes, network requests, or device calls occur.
"""
from pathlib import Path
import ctypes
import errno
import hashlib
import json
import os
import struct
import subprocess
import tempfile

BASE = Path(tempfile.mkdtemp(prefix='nbdriver-patch-tests-'))
ROOT = Path(__file__).resolve().parents[1]
FIX = Path(tempfile.mkdtemp(prefix='fixtures-', dir=BASE))
results = []

def result(name, passed, **evidence):
    results.append(dict(name=name, passed=bool(passed), **evidence))
    print(('PASS ' if passed else 'FINDING ') + name + ': ' + json.dumps(evidence, ensure_ascii=False))

def run(command):
    print('COMMAND ' + json.dumps(command), flush=True)
    subprocess.run(command, check=True)

def elf(data):
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, header[6] + i * 64)
                for i in range(header[12])]
    names = sections[header[13]]
    strings = data[names[4]:names[4] + names[5]]
    return {strings[s[0]:].split(b'\0', 1)[0].decode(): s for s in sections}

def section(data, name):
    s = elf(data)[name]
    return data[s[4]:s[4] + s[5]]

def symbols(data):
    sections = elf(data)
    names = section(data, '.strtab')
    return {names[s[0]:].split(b'\0', 1)[0].decode(): s
            for s in struct.iter_unpack('<IBBHQQ', section(data, '.symtab'))}

def main():
    source = FIX / 'input.S'
    release = '6.6.old-vendor'
    features = ' SMP preempt mod_unload modversions aarch64'
    source.write_text('''\
.text
.globl init_module
init_module: ret
.data
.quad allowed_symbol
.quad ordinary_import
.section .modinfo,"a",@progbits
.asciz "license=GPL"
.asciz "description=audit-only"
.asciz "name=nbdriver"
.asciz "vermagic=''' + release + features + '''"
.section .late_data,"aw",@progbits
.balign 64
.quad 0x1122334455667788
''')
    original = FIX / 'original.ko'
    run([os.environ.get('CLANG', 'clang-18'), '--target=aarch64-linux-gnu', '-c', str(source), '-o', str(original)])
    library = BASE / 'libnbdriver-patch.so'
    run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
         str(ROOT / 'tools/nbdriver_patch.c'), '-o', str(library)])
    lib = ctypes.CDLL(str(library))
    magic = lib.nbdriver_patch_vermagic
    magic.argtypes = [ctypes.c_char_p] * 3 + [ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_size_t]
    magic.restype = ctypes.c_int
    sym = lib.nbdriver_patch_symbols
    sym.argtypes = [ctypes.c_char_p] * 3 + [ctypes.POINTER(ctypes.c_char_p), ctypes.c_size_t,
                                         ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_size_t]
    sym.restype = ctypes.c_int
    raw = original.read_bytes()
    before_hash = hashlib.sha256(raw).hexdigest()
    counter = 0

    def rewrite(value, input_path=original, output_path=None):
        nonlocal counter
        counter += 1
        output = output_path or FIX / f'vermagic-{counter}.ko'
        changed = ctypes.c_int()
        log = ctypes.create_string_buffer(256)
        rc = magic(os.fsencode(input_path), os.fsencode(output), value.encode(),
                   ctypes.byref(changed), log, len(log))
        return rc, changed.value, log.value.decode(), output

    for label, new in [('longer', release + '-longer-build'), ('shorter', '6.6'),
                       ('equal_length', 'x' * len(release))]:
        rc, changed, log, output = rewrite(new)
        data = output.read_bytes() if not rc else b''
        info = section(data, '.modinfo') if data else b''
        result('release_' + label + '_preserves_space_features',
               rc == 0 and ('vermagic=' + new + features).encode() + b'\0' in info,
               rc=rc, changed=changed, log=log)
        result('other_modinfo_entries_' + label,
               b'license=GPL\0' in info and b'description=audit-only\0' in info)
    rc, changed, log, output = rewrite(release)
    result('unchanged_release_reports_zero', rc == 0 and changed == 0, rc=rc, changed=changed)
    for invalid in ['', '6.6 bad', '6.6\tbad', '6.6\nbad', '6.6\rbad', '6.6\fbad', '6.6\vbad']:
        rc, _, log, output = rewrite(invalid)
        result('reject_invalid_release_' + repr(invalid), rc == -errno.EINVAL and not output.exists(), rc=rc)
    rc, _, _, output = rewrite(release + 'x')
    old_late, new_late = elf(raw)['.late_data'], elf(output.read_bytes())['.late_data']
    result('preserve_64_byte_section_alignment',
           rc == 0 and new_late[4] % new_late[8] == 0,
           old_offset=old_late[4], new_offset=new_late[4], alignment=new_late[8],
           remainder=new_late[4] % new_late[8])
    result('late_section_bytes_unchanged', section(raw, '.late_data') == section(output.read_bytes(), '.late_data'))
    saved = output.read_bytes()
    rc, _, log, _ = rewrite('6.6.another', output_path=output)
    result('exclusive_output', rc == -errno.EEXIST and output.read_bytes() == saved, rc=rc, log=log)
    signed = FIX / 'signed.ko'
    signed.write_bytes(raw + b'~Module signature appended~\n')
    rc, _, log, output = rewrite('6.6.new', input_path=signed)
    result('reject_signed_vermagic', rc == -getattr(errno, 'EKEYREJECTED', 129) and not output.exists(), rc=rc)
    target = FIX / 'symlink-target'
    target.write_bytes(b'do not overwrite')
    link = FIX / 'symlink-output'
    link.symlink_to(target)
    rc, _, log, _ = rewrite('6.6.new', output_path=link)
    result('symlink_output_does_not_overwrite', rc < 0 and target.read_bytes() == b'do not overwrite', rc=rc)

    def rewrite_symbols(label, dump, allowed, input_path=original):
        path = FIX / (label + '.kallsyms')
        path.write_text(dump)
        output = FIX / (label + '.ko')
        count = ctypes.c_int()
        names = (ctypes.c_char_p * len(allowed))(*(n.encode() for n in allowed))
        log = ctypes.create_string_buffer(256)
        rc = sym(os.fsencode(input_path), os.fsencode(output), os.fsencode(path), names,
                 len(allowed), ctypes.byref(count), log, len(log))
        return rc, count.value, log.value.decode(), output

    dump = 'ffff000000001000 T allowed_symbol\nffff000000002000 T ordinary_import\n'
    rc, count, log, output = rewrite_symbols('allowlist', dump, ['allowed_symbol'])
    syms = symbols(output.read_bytes()) if rc == 0 else {}
    result('only_allowlisted_undefined_symbol_rewritten', rc == 0 and count == 1 and
           syms['allowed_symbol'][3] == 0xfff1 and syms['allowed_symbol'][4] == 0xffff000000001000 and
           syms['ordinary_import'][3] == 0, rc=rc, count=count)
    rc, _, log, output = rewrite_symbols('empty-allow', dump, [])
    result('reject_empty_allowlist', rc == -errno.EINVAL and not output.exists(), rc=rc)
    rc, _, log, output = rewrite_symbols('missing-allow', 'ffff000000002000 T ordinary_import\n', ['allowed_symbol'])
    result('reject_missing_allowlisted_symbol', rc == -errno.ENOENT and not output.exists(), rc=rc)
    rc, _, log, output = rewrite_symbols('signed-symbols', dump, ['allowed_symbol'], signed)
    result('reject_signed_symbols', rc == -getattr(errno, 'EKEYREJECTED', 129) and not output.exists(), rc=rc)
    rc, _, log, output = rewrite_symbols('module-owned', 'ffff000000001000 T allowed_symbol [other_module]\n', ['allowed_symbol'])
    result('reject_module_owned_symbol_address', rc < 0 and not output.exists(), rc=rc, log=log)
    rc, _, log, output = rewrite_symbols('duplicate-symbol', 'ffff000000001000 t allowed_symbol\nffff000000003000 T allowed_symbol\n', ['allowed_symbol'])
    result('reject_ambiguous_symbol_address', rc < 0 and not output.exists(), rc=rc, log=log,
           chosen_address=hex(symbols(output.read_bytes())['allowed_symbol'][4]) if output.exists() else None)
    result('original_sha256_unchanged', hashlib.sha256(original.read_bytes()).hexdigest() == before_hash,
           sha256=before_hash)

    (BASE / 'probe-results.json').write_text(json.dumps(results, indent=2, ensure_ascii=False) + '\n')
    print(f'RESULT {sum(r["passed"] for r in results)} checks passed; '
          f'{sum(not r["passed"] for r in results)} failures.')
    if any(not r['passed'] for r in results):
        raise SystemExit(1)

if __name__ == '__main__':
    main()
