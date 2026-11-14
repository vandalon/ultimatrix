#!/usr/bin/env python3
"""Fails the build when a face reserves too much static RAM (.dram0.data + .dram0.bss).

Static RAM is reserved in every boot, including the update-only boot that never loads the face, and it comes
straight out of the heap that an update download needs. Big face objects (feed tickers, snapshots, caches) belong
on the heap, created when the face starts (see docs/UPDATES.md, "Memory").

usage: check-static-ram.py <ultimatrix.elf> <objdump path> <mode> [budget in KB] [bt-release]

With "bt-release" (faces that carry the Bluetooth proxy) the memory that esp_bt_controller_mem_release() hands back to the heap at
boot while the proxy is off (the _bt_*_start.._end ranges) is not counted: it is only reserved while the proxy is switched on.
"""
import re
import subprocess
import sys

DEFAULT_BUDGET_KB = 62  # Football was 73 KB before its ticker moved to the heap (now ~57 KB); the smaller faces are ~53 KB


def main():
    elf, objdump, mode = sys.argv[1:4]
    budget = int(float(sys.argv[4]) * 1024) if len(sys.argv) > 4 else DEFAULT_BUDGET_KB * 1024
    size_tool = re.sub(r"objdump(\.exe)?$", r"size\1", objdump)
    out = subprocess.run([size_tool, "-A", elf], capture_output=True, text=True, check=True).stdout
    used = sum(int(m.group(1)) for m in re.finditer(r"^\.dram0\.(?:data|bss)\s+(\d+)", out, re.M))
    note = ""
    if len(sys.argv) > 5 and sys.argv[5] == "bt-release":
        nm = re.sub(r"objdump(\.exe)?$", r"nm\1", objdump)
        sym = {m.group(2): int(m.group(1), 16) for m in re.finditer(r"^([0-9a-f]+) \w (_bt_\w+)$", subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout, re.M)}
        freed = sum(sym[e] - sym[s] for s, e in (("_bt_data_start", "_bt_data_end"), ("_bt_controller_data_start", "_bt_controller_data_end"),
                                                 ("_bt_bss_start", "_bt_bss_end"), ("_bt_controller_bss_start", "_bt_controller_bss_end")) if s in sym and e in sym)
        used -= freed
        note = f" (without {freed / 1024:.1f} KB of Bluetooth memory that is returned to the heap while the proxy is off)"
    print(f"[static RAM] {mode}: {used / 1024:.1f} KB of {budget / 1024:.0f} KB budget{note}")
    if used > budget:
        print(f"ERROR: {mode} reserves {used / 1024:.1f} KB of static RAM, over the {budget / 1024:.0f} KB budget.\n"
              "Move large objects (tickers, snapshots, caches, buffers) to the heap and create them when the face starts,\n"
              "so the update-only boot does not reserve them. Inspect with: xtensa-esp32-elf-nm -S --size-sort -C <elf>",
              file=sys.stderr)
        return 1
    return 0


sys.exit(main())
