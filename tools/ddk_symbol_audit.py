#!/usr/bin/env python3
"""Record actual DDK export, config and source evidence without patching a kernel."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

SYMBOLS = [
    "anon_inode_getfile", "get_unused_fd_flags", "put_unused_fd", "fd_install",
    "fput", "task_work_add", "register_kprobe", "unregister_kprobe",
    "kallsyms_lookup_name", "sys_call_table", "__arm64_sys_ni_syscall",
    "synchronize_rcu_tasks", "synchronize_rcu_tasks_rude", "kimage_voffset",
    "sys_ni_syscall", "copy_from_kernel_nofault", "copy_to_kernel_nofault", "stop_machine", "__set_fixmap",
    "dcache_clean_inval_poc", "__flush_dcache_area", "init_mm",
    "__arm64_sys_reboot", "__se_sys_reboot", "__arch_copy_to_user",
    "mm_access", "fixup_user_fault", "zero_pfn", "copy_to_user_page",
    "init_user_ns", "get_task_cred", "__put_cred", "task_uid",
    "__arm64_sys_lookup_dcookie", "__arm64_sys_ni_syscall",
]
SOURCE_PATHS = [
    "Makefile", "kernel/kallsyms.c", "kernel/task_work.c", "kernel/kprobes.c",
    "mm/gup.c", "mm/memory.c", "arch/arm64/mm/flush.c", "include/linux/mm.h",
    "kernel/rcu/tasks.h", "kernel/stop_machine.c", "mm/vmalloc.c",
    "arch/arm64/include/asm/memory.h", "include/linux/vmalloc.h",
    "kernel/reboot.c", "fs/anon_inodes.c", "fs/file.c", "mm/maccess.c",
    "arch/arm64/kernel/sys.c", "arch/arm64/kernel/syscall.c",
    "arch/arm64/include/asm/syscall_wrapper.h", "arch/arm64/include/asm/cacheflush.h",
    "include/linux/task_work.h", "android/abi_gki_aarch64_generic",
    "include/linux/cred.h", "include/linux/user_namespace.h",
    "kernel/cred.c", "kernel/user.c", "kernel/user_namespace.c",
    "scripts/syscall.tbl", "include/uapi/asm-generic/unistd.h",
    "android/abi_gki_aarch64_qcom",
]
CONFIGS = [
    "CONFIG_ARM64", "CONFIG_MODULES", "CONFIG_MODVERSIONS", "CONFIG_MODULE_SIG",
    "CONFIG_MODULE_SIG_FORCE", "CONFIG_KPROBES", "CONFIG_KALLSYMS", "CONFIG_KALLSYMS_ALL",
    "CONFIG_LTO_CLANG", "CONFIG_LTO_CLANG_THIN", "CONFIG_CFI_CLANG", "CONFIG_CFI_PERMISSIVE",
    "CONFIG_ARM64_4K_PAGES", "CONFIG_ARM64_16K_PAGES", "CONFIG_ARM64_64K_PAGES",
    "CONFIG_TASKS_RCU", "CONFIG_TASKS_RUDE_RCU",
    "CONFIG_STRICT_KERNEL_RWX", "CONFIG_RANDOMIZE_BASE", "CONFIG_TRIM_UNUSED_KSYMS",
    "CONFIG_UNUSED_KSYMS_WHITELIST", "CONFIG_ANDROID_KABI_RESERVE",
]


def record(path):
    data = path.read_bytes()
    return {"path": str(path), "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kdir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--module", type=Path)
    args = parser.parse_args()
    root = args.kdir.resolve()
    report = {
        "date_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "kmi": os.environ.get("KMI"), "kdir": str(root),
        "image_reference": os.environ.get("DDK_IMAGE_REFERENCE"),
        "source_commit": os.environ.get("GITHUB_SHA"),
        "limits": "DDK evidence only; source occurrence or exported symbol does not prove vendor availability or runtime probe success.",
        "symvers": [], "system_maps": [], "configs": [], "source": [], "commands": [],
    }
    all_exports = set()
    ddk_root = Path(os.environ.get("DDK_ROOT", "/opt/ddk"))
    roots = list(dict.fromkeys([root, root / "source", root / "build", root.parent / "common", ddk_root / "src" / root.name]))
    for directory in roots:
        for name in ("Module.symvers", "System.map", ".config"):
            path = directory / name
            if not path.is_file():
                continue
            entry = record(path)
            text = path.read_text(errors="replace")
            if name == "Module.symvers":
                all_exports.update(line.split()[1] for line in text.splitlines() if len(line.split()) > 1)
                entry["symbols"] = {symbol: [line for line in text.splitlines() if len(line.split()) > 1 and line.split()[1] == symbol] for symbol in SYMBOLS}
                report["symvers"].append(entry)
            elif name == "System.map":
                entry["symbols"] = {symbol: [line for line in text.splitlines() if line.split() and line.split()[-1] == symbol] for symbol in SYMBOLS}
                report["system_maps"].append(entry)
            else:
                entry["values"] = {key: next((line for line in text.splitlines() if line.startswith(key + "=") or line == "# " + key + " is not set"), "not recorded") for key in CONFIGS}
                report["configs"].append(entry)
    for relative in SOURCE_PATHS:
        candidates = [directory / relative for directory in roots]
        path = next((candidate for candidate in candidates if candidate.is_file()), None)
        if path is None:
            report["source"].append({"relative": relative, "status": "not found"})
            continue
        entry = record(path)
        lines = path.read_text(errors="replace").splitlines()
        entry["relative"] = relative
        entry["evidence"] = [{"line": number, "text": line} for number, line in enumerate(lines, 1) if any(re.search(r"\b" + re.escape(symbol) + r"\b", line) for symbol in SYMBOLS) or "__SYSCALL_DEFINEx" in line or "invoke_syscall" in line]
        report["source"].append(entry)
    commands = [["make", "-s", "-C", str(root), "kernelrelease"], ["clang", "--version"]]
    module_nm = None
    if args.module and args.module.is_file():
        report["module"] = record(args.module)
        for names, flags in [(("llvm-nm", "nm"), ["-u"]), (("llvm-readelf", "readelf"), ["-p", ".modinfo"])]:
            tool = next((shutil.which(name) for name in names if shutil.which(name)), None)
            if tool:
                commands.append([tool, *flags, str(args.module)])
                if flags == ["-u"]:
                    module_nm = tool
    for command in commands:
        try:
            result = subprocess.run(command, text=True, capture_output=True, timeout=30)
            report["commands"].append({"argv": command, "stdout": result.stdout, "stderr": result.stderr, "exit_code": result.returncode})
            if module_nm and command[0] == module_nm and result.returncode == 0:
                imports = sorted({line.split()[-1] for line in result.stdout.splitlines() if line.split()})
                report["module_imports"] = imports
                report["module_imports_absent_from_symvers"] = sorted(set(imports) - all_exports) if report["symvers"] else None
        except (OSError, subprocess.TimeoutExpired) as exc:
            report["commands"].append({"argv": command, "error": str(exc)})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print("DDK_AUDIT", args.output, "symvers=" + str(len(report["symvers"])), "source_files=" + str(sum("sha256" in item for item in report["source"])))
    if report.get("module_imports_absent_from_symvers"):
        print("DDK_AUDIT unresolved module exports:", ", ".join(report["module_imports_absent_from_symvers"]))


if __name__ == "__main__":
    main()
