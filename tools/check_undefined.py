#!/usr/bin/env python3
# 只检查真实模块静态导入与给定 symvers/禁止清单；不证明动态调用 ABI 或厂商实机可用。
"""Fail the build when a .ko imports a symbol the kernel does not export.

A module links successfully with undefined symbols; the failure only shows up
at insmod time as "Unknown symbol". Checking Module.symvers here turns that
runtime failure into a build failure with the symbol names printed.
"""
import argparse
import pathlib
import re
import subprocess
import sys


def read_exports(path):
    exports = {}
    for line in path.read_text().splitlines():
        parts = line.split("\t")
        if len(parts) < 4:
            continue
        crc, name, module, export_type = parts[0], parts[1], parts[2], parts[3]
        exports[name] = (module, export_type)
    return exports


def read_imports(module, nm):
    result = subprocess.run([nm, "-u", str(module)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"{nm} failed: {result.stderr.strip()}")
    names = set()
    for line in result.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        name = line.split()[-1]
        # __this_module 由模块自身提供，不是内核导出项。
        if name != "__this_module":
            names.add(name)
    return sorted(names)


def pick_nm(explicit):
    if explicit:
        return explicit
    for candidate in ("llvm-nm", "llvm-nm-18", "nm"):
        from shutil import which
        if which(candidate):
            return candidate
    raise SystemExit("no nm/llvm-nm available")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--symvers", required=True)
    parser.add_argument("--module", required=True)
    parser.add_argument("--kmi", default="unknown")
    parser.add_argument("--nm", default=None)
    args = parser.parse_args()

    symvers = pathlib.Path(args.symvers)
    module = pathlib.Path(args.module)
    if not symvers.is_file():
        raise SystemExit(f"Module.symvers not found: {symvers}")
    if not module.is_file():
        raise SystemExit(f"module not found: {module}")

    exports = read_exports(symvers)
    imports = read_imports(module, pick_nm(args.nm))

    protected = {
        "empty_zero_page", "init_mm", "sys_call_table",
        "kallsyms_lookup_name", "task_work_add", "__set_fixmap",
        "copy_to_kernel_nofault", "__arm64_sys_reboot",
        # Optional table-entry dependencies must stay dynamically resolved.
        "stop_machine", "copy_from_kernel_nofault", "kimage_voffset",
        "synchronize_rcu_tasks", "synchronize_rcu_tasks_rude",
        # Observed absent from vendor 6.1.141 exports despite the DDK export.
        "xas_find_marked",
        # Observed absent in Android 12 vendor 5.10 module exports.
        "init_user_ns", "get_task_cred", "__put_cred",
        # Observed absent on Xiaomi Android 13 vendor 5.15.78 exports.
        "sock_from_file",
    }
    missing = [name for name in imports if name not in exports or name in protected]

    print(f"KMI={args.kmi}")
    print(f"module={module}")
    print(f"symvers={symvers}")
    print(f"exports={len(exports)} imports={len(imports)}")
    for name in imports:
        mark = "MISSING" if name in missing else "ok"
        module_name, export_type = exports.get(name, ("-", "-"))
        if name in protected:
            mark = "MISSING"
            export_type = "PROTECTED"
        print(f"  {mark:7} {name:34} {module_name:8} {export_type}")

    if missing:
        print()
        print("UNRESOLVED_OR_PROTECTED: " + ", ".join(missing))
        print("这些符号不能作为模块静态导入，可能导致 Unknown symbol 或受保护符号拒绝加载。")
        return 1

    print()
    print("UNRESOLVED: none")
    return 0


if __name__ == "__main__":
    sys.exit(main())
