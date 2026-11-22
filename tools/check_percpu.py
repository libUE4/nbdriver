#!/usr/bin/env python3
"""Reject static per-CPU storage in the final ARM64 kernel module.

Module loading uses the finite boot-time reserved per-CPU area for these
sections, before module init can handle allocation failure. Use dynamic
alloc_percpu instead; check actual ELF, including LTO output.
"""
import argparse
from pathlib import Path
import struct


def static_percpu_sections(data):
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
    result = []
    for section in sections:
        name_offset = section[0]
        end = names.find(b"\0", name_offset)
        if name_offset >= len(names) or end < 0:
            raise ValueError("invalid section name")
        name = names[name_offset:end].decode("ascii")
        if name.startswith(".data..percpu") and section[5]:
            result.append((name, section[5]))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", required=True, type=Path)
    args = parser.parse_args()
    try:
        sections = static_percpu_sections(args.module.read_bytes())
    except (ValueError, OSError, struct.error) as error:
        parser.exit(1, "FAIL: " + str(error) + "\n")
    if sections:
        parser.exit(1, "FAIL: static per-CPU sections consume reserved module space: " + repr(sections) + "\n")
    print("PASS: final module requires zero static per-CPU bytes")


if __name__ == "__main__":
    main()
