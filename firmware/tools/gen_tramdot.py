#!/usr/bin/env python3
"""Generates lib/faces/TramDot.h from the TramDot glyph table (build_font.py in the TramDot font folder).
Usage: gen_tramdot.py [path/to/build_font.py]   (default: ~/scripts/tram_font/dist/build_font.py)"""
import importlib.util, os, sys
src = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/scripts/tram_font/dist/build_font.py")
spec = importlib.util.spec_from_file_location("bf", src); bf = importlib.util.module_from_spec(spec); spec.loader.exec_module(bf)
chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 :-./&'!"
rows_out = []
for ch in chars:
    pts, w = bf.dots(ch); s = set(map(tuple, pts)); rows = []
    for r in range(6, -1, -1):
        rows.append(sum(1 << (w - 1 - c) for c in range(w) if (c, r) in s))
    tail = sum(1 << (w - 1 - c) for c in range(w) if (c, -1) in s)  # the one row below the baseline (the Q's tail)
    rows_out.append("    {%d, {%s}, 0x%02X},  // %s" % (w, ", ".join("0x%02X" % v for v in rows), tail, ch))
head = open(os.path.join(os.path.dirname(__file__), "..", "lib/faces/TramDot.h")).read().split("  static const TramGlyph G[] = {")
tail = head[1].split("  };", 1)[1]
open(os.path.join(os.path.dirname(__file__), "..", "lib/faces/TramDot.h"), "w").write(head[0] + "  static const TramGlyph G[] = {\n" + "\n".join(rows_out) + "\n  };" + tail)
