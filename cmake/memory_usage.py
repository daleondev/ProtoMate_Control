#!/usr/bin/env python3
"""Report linker-region occupancy from the final ELF and GNU ld memory map."""
import argparse
from pathlib import Path
import re
import subprocess


def memory_usage(map_text, sections_text):
    configuration = map_text.split("Memory Configuration\n", 1)[1].split(
        "Linker script and memory map", 1)[0]
    regions = {}
    for name, origin, length in re.findall(
            r"^([^\s*]+)\s+0x([\da-fA-F]+)\s+0x([\da-fA-F]+)", configuration, re.M):
        regions[name] = [int(origin, 16), int(length, 16), 0]
    if not regions:
        raise ValueError("No named linker memory regions found")
    sections = re.findall(
        r"^\s*\d+\s+\S+\s+([\da-fA-F]+)\s+([\da-fA-F]+)\s+([\da-fA-F]+)"
        r"[^\n]*\n\s*([^\n]*)", sections_text, re.M)
    allocated = 0
    for size, vma, lma, flags in sections:
        flags = set(flags.replace(",", " ").split())
        size, vma, lma = (int(value, 16) for value in (size, vma, lma))
        if "ALLOC" not in flags or size == 0:
            continue
        allocated += 1
        # Initialized RAM consumes both run-address RAM and load-address flash.
        # NOLOAD heap/stack/BSS consume only their run-address region.
        for address in {vma, lma} if "LOAD" in flags else {vma}:
            matches = [region for region in regions.values()
                       if region[0] <= address < region[0] + region[1]]
            if len(matches) != 1:
                raise ValueError(f"Section at {address:#x} does not select exactly one memory region")
            region = matches[0]
            used = address + size - region[0]
            if used > region[1]:
                raise ValueError(f"Section overflows memory region at {address:#x}")
            region[2] = max(region[2], used)
    if not allocated:
        raise ValueError("No allocated ELF sections found")
    return {name: (used, length) for name, (_, length, used) in regions.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--map", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--objdump", required=True)
    args = parser.parse_args()
    sections = subprocess.check_output([args.objdump, "-h", str(args.elf)], text=True)
    usage = memory_usage(args.map.read_text(), sections)
    print(f"{args.elf.name} — linked memory usage")
    print(f"{'Memory region':>15} {'Used bytes':>12} {'Region bytes':>14} {'% used':>10}")
    for name, (used, capacity) in usage.items():
        percent = 100 * used / capacity if capacity else 0
        print(f"{name:>15} {used:>12} {capacity:>14} {percent:>9.2f}%")


if __name__ == "__main__":
    main()
