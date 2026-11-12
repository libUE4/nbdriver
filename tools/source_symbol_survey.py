#!/usr/bin/env python3
"""Fetch pinned public kernel sources; record observations, never load modules."""
import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import urllib.error
import urllib.parse
import urllib.request

PATHS = [
    "Makefile", "fs/anon_inodes.c", "fs/file.c", "fs/file_table.c",
    "kernel/task_work.c", "include/linux/task_work.h", "kernel/kprobes.c",
    "kernel/kallsyms.c", "kernel/reboot.c", "arch/arm64/kernel/sys.c",
    "arch/arm64/kernel/syscall.c", "arch/arm64/include/asm/syscall_wrapper.h",
    "arch/arm64/mm/fixmap.c", "arch/arm64/include/asm/cacheflush.h",
    "mm/maccess.c", "kernel/stop_machine.c", "arch/arm64/configs/gki_defconfig",
    "android/abi_gki_aarch64_generic", "android/abi_gki_aarch64_qcom",
]
SYMBOLS = [
    "anon_inode_getfile", "get_unused_fd_flags", "put_unused_fd", "fd_install",
    "fput", "task_work_add", "register_kprobe", "unregister_kprobe",
    "kallsyms_lookup_name", "sys_call_table", "__arm64_sys_ni_syscall",
    "sys_ni_syscall", "copy_to_kernel_nofault", "stop_machine", "__set_fixmap",
    "dcache_clean_inval_poc", "__flush_dcache_area", "__arm64_sys_reboot",
    "__se_sys_reboot", "TWA_RESUME", "SYSCALL_DEFINE4", "__cfi_slowpath",
]


def fetch(url, authenticated=False):
    headers = {"User-Agent": "nbdriver-source-audit", "Accept": "application/vnd.github+json"}
    if authenticated and os.environ.get("GITHUB_TOKEN"):
        headers["Authorization"] = "Bearer " + os.environ["GITHUB_TOKEN"]
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=20) as response:
        return response.read()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("targets", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    targets = json.loads(args.targets.read_text())
    records = []
    for target in targets:
        record = dict(target)
        try:
            api = "https://api.github.com/repos/" + target["repo"] + "/commits/" + urllib.parse.quote(target["ref"], safe="")
            body = fetch(api, authenticated=True)
            commit = json.loads(body)["sha"]
            record["commit"] = commit
            directory = args.output / target["id"]
            directory.mkdir(exist_ok=True)
            (directory / "commit.json").write_bytes(body)
            print(target["id"], commit, flush=True)
        except Exception as exc:
            record["resolution_error"] = str(exc)
            print(target["id"], "UNRESOLVED", str(exc), flush=True)
        records.append(record)

    def download(job):
        target, path = job
        relative = target["id"] + "/" + path
        local = args.output / relative
        url = "https://raw.githubusercontent.com/" + target["repo"] + "/" + target["commit"] + "/" + path
        result = {"target": target["id"], "path": path, "url": url, "local": relative}
        try:
            body = fetch(url)
            local.parent.mkdir(parents=True, exist_ok=True)
            local.write_bytes(body)
            result.update(status="downloaded", size=len(body), sha256=hashlib.sha256(body).hexdigest())
            lines = body.decode(errors="replace").splitlines()
            result["evidence"] = [
                {"line": number, "text": line, "symbols": [symbol for symbol in SYMBOLS if re.search(r"\b" + re.escape(symbol) + r"\b", line)]}
                for number, line in enumerate(lines, 1)
                if any(re.search(r"\b" + re.escape(symbol) + r"\b", line) for symbol in SYMBOLS)
            ]
            result["exports"] = [
                {"line": number, "text": line.strip(), "symbol": match.group(1)}
                for number, line in enumerate(lines, 1)
                for match in [re.search(r"EXPORT_SYMBOL(?:_GPL|_NS|_NS_GPL)?\s*\(\s*(\w+)", line)]
                if match and match.group(1) in SYMBOLS
            ]
        except urllib.error.HTTPError as exc:
            result.update(status="http_error", http_status=exc.code)
        except Exception as exc:
            result.update(status="download_error", error=str(exc))
        return result

    jobs = [(target, path) for target in records if "commit" in target for path in PATHS]
    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
        files = list(pool.map(download, jobs))
    report = {
        "date_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "method": "Pinned GitHub commit source inspection; absence is scoped to downloaded files; no device or export-table validation implied.",
        "targets": records, "files": files,
    }
    (args.output / "survey.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    for target in records:
        found = [item for item in files if item["target"] == target["id"] and item["status"] == "downloaded"]
        exports = sorted({entry["symbol"] for item in found for entry in item["exports"]})
        print(target["id"], "files=" + str(len(found)), "exports=" + ",".join(exports), flush=True)
    print("Evidence:", args.output / "survey.json")


if __name__ == "__main__":
    main()
