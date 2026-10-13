#!/usr/bin/env python3
"""Render the boot-screen U logo as LED-matrix app icons (pure stdlib; same geometry as BootScreen.cpp).

Usage: gen_icons.py [output dir]   (default: firmware/web/icons)
"""
import math, struct, sys, zlib
from pathlib import Path

SIZE, CENTRE_Y, OUTER_X, OUTER_Y, INNER_X, INNER_Y = 64, 40, 28, 22, 16, 14
CYAN = (70, 210, 255)
BG = (5, 8, 10)


def level(x, y):  # BootScreen.cpp backdropLevel(x, y, glow = 255): the U fully lit, other LEDs barely on
    dx, dy = x - 32.0, y - CENTRE_Y
    outer = abs(dx) <= OUTER_X if y <= CENTRE_Y else dx * dx / OUTER_X ** 2 + dy * dy / OUTER_Y ** 2 <= 1
    inner = abs(dx) <= INNER_X if y <= CENTRE_Y else dx * dx / INNER_X ** 2 + dy * dy / INNER_Y ** 2 <= 1
    return 255 - 105 * y // SIZE if outer and not inner else 10


def smooth(a, b, x):
    t = min(1.0, max(0.0, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)


def render(px, scale):
    pitch = px * scale / 32  # 32x32 LEDs on every other panel pixel
    off = px * (1 - scale) / 2
    rows = []
    for py in range(px):
        row = bytearray([0])
        for pxx in range(px):
            gx, gy = (pxx - off) / pitch, (py - off) / pitch
            cx, cy = min(31, max(0, math.floor(gx))), min(31, max(0, math.floor(gy)))
            best = 0.0
            for ox in (-1, 0, 1):  # neighbours too, so the glow bleeds across cells
                for oy in (-1, 0, 1):
                    ix, iy = cx + ox, cy + oy
                    if not (0 <= ix < 32 and 0 <= iy < 32): continue
                    lv = level(2 * ix + 1, 2 * iy + 1) / 255
                    d = math.hypot(gx - (ix + .5), gy - (iy + .5))
                    core = smooth(.46, .34, d) * lv
                    glow = .30 * math.exp(-(d / .85) ** 2) * lv * lv
                    best = max(best, core + glow)
            best = min(1.0, best)
            hot = best ** 4 * .55  # whitish centre on the bright LEDs
            row += bytes(min(255, int(BG[i] + (CYAN[i] - BG[i]) * best + (255 - CYAN[i]) * hot)) for i in range(3))
        rows.append(bytes(row))
    return rows


def png(rows, px):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", px, px, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "web" / "icons"
out.mkdir(parents=True, exist_ok=True)
for name, px, scale in (("icon-192", 192, .8), ("icon-512", 512, .8), ("icon-maskable-192", 192, .62),
                        ("apple-touch-icon", 180, .8), ("favicon-32", 32, .92)):
    (out / f"{name}.png").write_bytes(png(render(px, scale), px))
    print(name)
