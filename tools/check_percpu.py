#!/usr/bin/env python3
"""Reject application static per-CPU storage in the final ARM64 kernel module.

Module loading uses the finite boot-time reserved per-CPU area for these
sections, before module init can handle allocation failure. Use dynamic
alloc_percpu for driver state. Retain kernel allocation-profiler counters;
validate their exact symbol type, size and byte coverage in the final ELF.
"""
import argparse
from pathlib import Path
import struct
import re


def inspect_percpu(data):
    if data[:7] != b"\x7fELF\x02\x01\x01" or len(data) < 64:
        raise ValueError("expected ELF64 little-endian module")
    if struct.unpack_from("<HH", data, 16) != (1, 183):
        raise ValueError("expected relocatable AArch64 module")
    offset = struct.unpack_from("<Q", data, 40)[0]
    size, count, names_index = struct.unpack_from("<HHH", data, 58)
    if size != 64 or not count or names_index >= count or offset + count * size > len(data):
        raise ValueError("invalid section table")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * size) for i in range(count)]
    names_section = sections[names_index]
    start, length = names_section[4:6]
    if start + length > len(data):
        raise ValueError("invalid section names")
    names = data[start:start + length]
    profiling = {}
    for table in sections:
        if table[1] != 2:
            continue
        if table[9] != 24 or table[5] % 24 or table[6] >= count or table[4] + table[5] > len(data):
            raise ValueError("invalid symbol table")
        strings = sections[table[6]]
        if strings[4] + strings[5] > len(data):
            raise ValueError("invalid symbol names")
        strings = data[strings[4]:strings[4] + strings[5]]
        for at in range(table[4], table[4] + table[5], 24):
            name, info, other, index, value, length = struct.unpack_from("<IBBHQQ", data, at)
            end = strings.find(b"\0", name)
            if name >= len(strings) or end < 0:
                raise ValueError("invalid symbol name")
            name = strings[name:end].decode("ascii")
            # Android 6.12 CONFIG_MEM_ALLOC_PROFILING generates a 16-byte
            # local object for each allocator call. These are kernel-owned
            # accounting data, not the driver's former 136-byte CPU cache.
            if info == 1 and length == 16 and re.fullmatch(r".+\._alloc_tag_cntr(?:\.\d+)?", name):
                if index >= count or value + length > sections[index][5]:
                    raise ValueError("profiling symbol outside section")
                profiling.setdefault(index, []).append((value, value + length))
    result = {"reserved_bytes": 0, "kernel_profiling_bytes": 0, "unexpected": []}
    for index, section in enumerate(sections):
        name_offset = section[0]
        end = names.find(b"\0", name_offset)
        if name_offset >= len(names) or end < 0:
            raise ValueError("invalid section name")
        name = names[name_offset:end].decode("ascii")
        if name.startswith(".data..percpu") and section[5]:
            result["reserved_bytes"] += section[5]
            covered = 0
            previous_end = 0
            for start, end in sorted(profiling.get(index, [])):
                if start < previous_end:
                    raise ValueError("overlapping profiling counters")
                covered += end - start
                previous_end = end
            result["kernel_profiling_bytes"] += covered
            if covered != section[5]:
                result["unexpected"].append((name, section[5] - covered))
    return result


def static_percpu_sections(data):
    return inspect_percpu(data)["unexpected"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = inspect_percpu(args.module.read_bytes())
        sections = report["unexpected"]
    except (ValueError, OSError, struct.error) as error:
        parser.exit(1, "FAIL: " + str(error) + "\n")
    if sections:
        parser.exit(1, "FAIL: static per-CPU sections consume reserved module space: " + repr(sections) + "\n")
    print("PASS: driver static per-CPU bytes=0; kernel profiling bytes=" + str(report["kernel_profiling_bytes"]) + "; total reserved bytes=" + str(report["reserved_bytes"]))


if __name__ == "__main__":
    main()
