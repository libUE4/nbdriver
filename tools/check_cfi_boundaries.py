#!/usr/bin/env python3
"""Check the final ARM64 ELF for the CFI regressions found on vendor kernels.

This verifies concrete call boundaries, not arbitrary vendor ABI compatibility.
Callback signatures are still checked by the compiler and kernel CFI/KCFI.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def inspect(module, objdump, nm):
    data = module.read_bytes()
    dis = subprocess.run([objdump, "-dr", str(module)], check=True,
                         capture_output=True, text=True).stdout
    symbols = subprocess.run([nm, "-u", str(module)], check=True,
                             capture_output=True, text=True).stdout
    imports = {line.split()[-1] for line in symbols.splitlines() if line.split()}
    parts = re.split(r"(?mi)^[0-9a-f]+ <([^>]+)>:\n", dis)
    functions = dict(zip(parts[1::2], parts[2::2]))
    errors, checks = [], []
    for name in ("class_for_each_device",):
        # A dynamic lookup of the raw body must never replace these exports.
        callers = [fn for fn, body in functions.items() if re.search(
            r"R_AARCH64_(?:CALL|JUMP)26\s+" + name + r"(?:\s|$)", body)]
        valid = name in imports and bool(callers)
        checks.append({"kind": "direct_export", "symbol": name,
                       "callers": callers, "pass": valid})
        if not valid:
            errors.append("missing direct exported call: " + name)
    for name in ("kern_path", "path_put", "iov_iter_advance"):
        valid = name not in imports
        checks.append({"kind": "vendor_optional_import", "symbol": name,
                       "pass": valid})
        if not valid:
            errors.append("vendor-unavailable path API became a static import: " + name)
    for name, body in functions.items():
        if not re.match(r"(?:nbdriver_(?:touch|kgsl|metadata)_init|"
                        r"nbdriver_proc_visibility_init)(?:[.$]|$)", name):
            continue
        invalid = bool(re.search(r"R_AARCH64_\w+\s+(?:__cfi_slowpath\w*|"
                                 r"__ubsan_handle_cfi_\w*)\b", body))
        checks.append({"kind": "init_cfi", "function": name, "pass": not invalid})
        if invalid:
            errors.append("CFI-checked indirect call in initialization: " + name)
    boundary = []
    for required in ("kgsl_info_call", "nbdriver_proc_path_lookup",
                     "nbdriver_proc_path_release", "gyro_iov_advance_call",
                     "config_open", "config_rename", "config_unlink",
                     "config_put_cred"):
        found = [(name, body) for name, body in functions.items()
                 if re.match(re.escape(required) + r"(?:[.$]|$)", name)]
        if not found:
            errors.append("missing isolated raw-symbol call boundary: " + required)
        boundary.extend(found)
    for name, body in boundary:
        # This tiny typed wrapper has one indirect call. Any sanitizer trap or
        # slowpath here recreates the raw-body failure; do not exempt the ioctl
        # or registered callbacks instead. noinline preserves this boundary.
        call = bool(re.search(r"\b(?:blr|br)\s+x\d+\b", body))
        guarded = bool(re.search(r"\bbrk\b|__cfi_slowpath|__ubsan_handle_cfi_", body))
        valid = call and not guarded
        checks.append({"kind": "private_abi_boundary", "function": name,
                       "indirect_transfer": call, "pass": valid})
        if not valid:
            errors.append("unsafe raw-body call boundary: " + name)
    if "sock_from_file" in imports:
        errors.append("vendor-unavailable sock_from_file became a static import")
    return {"module": str(module), "sha256": hashlib.sha256(data).hexdigest(),
            "checks": checks, "errors": errors, "pass": not errors,
            "limits": "ELF call checks only; no claim of vendor runtime or all crash paths."}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--module", required=True, type=Path)
    p.add_argument("--output", type=Path)
    p.add_argument("--objdump")
    p.add_argument("--nm")
    a = p.parse_args()
    objdump = a.objdump or shutil.which("llvm-objdump") or shutil.which("llvm-objdump-18")
    nm = a.nm or shutil.which("llvm-nm") or shutil.which("llvm-nm-18")
    if not objdump or not nm:
        p.error("LLVM objdump and nm are required")
    result = inspect(a.module, objdump, nm)
    if a.output:
        a.output.parent.mkdir(parents=True, exist_ok=True)
        a.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
