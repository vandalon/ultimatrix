#!/usr/bin/env python3
"""Fails the build when a face reserves too much static RAM (.dram0.data + .dram0.bss).

Static RAM is reserved in every boot, including the update-only boot that never loads the face, and it comes
straight out of the heap that an update download needs. Big face objects (feed tickers, snapshots, caches) belong
on the heap, created when the face starts (see docs/UPDATES.md, "Memory").

usage: check-static-ram.py <ultimatrix.elf> <objdump path> <mode> [budget in KB]
"""
import re
import subprocess
import sys

DEFAULT_BUDGET_KB = 60  # Football was 73 KB before its ticker moved to the heap (now ~57 KB); the smaller faces are ~53 KB


def main():
    elf, objdump, mode = sys.argv[1:4]
    budget = int(float(sys.argv[4]) * 1024) if len(sys.argv) > 4 else DEFAULT_BUDGET_KB * 1024
    size_tool = re.sub(r"objdump(\.exe)?$", r"size\1", objdump)
    out = subprocess.run([size_tool, "-A", elf], capture_output=True, text=True, check=True).stdout
    used = sum(int(m.group(1)) for m in re.finditer(r"^\.dram0\.(?:data|bss)\s+(\d+)", out, re.M))
    print(f"[static RAM] {mode}: {used / 1024:.1f} KB of {budget / 1024:.0f} KB budget")
    if used > budget:
        print(f"ERROR: {mode} reserves {used / 1024:.1f} KB of static RAM, over the {budget / 1024:.0f} KB budget.\n"
              "Move large objects (tickers, snapshots, caches, buffers) to the heap and create them when the face starts,\n"
              "so the update-only boot does not reserve them. Inspect with: xtensa-esp32-elf-nm -S --size-sort -C <elf>",
              file=sys.stderr)
        return 1
    return 0


sys.exit(main())
