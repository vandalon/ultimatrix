#!/usr/bin/env python3
"""Render the Home Assistant face's screens (home page, energy page, alerts, status) with the real face code and made-up values.

Writes scene PNGs and a contact sheet; needs a C++ compiler and ffmpeg (no Pillow)."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
FIRMWARE = HERE.parent.parent
REPO = FIRMWARE.parent


def build(work):
    gfx = FIRMWARE / "vendor/gfx"
    binary = work / "home-preview"
    command = [os.environ.get("CXX", "c++"), "-std=c++17", "-O1", "-DARDUINO=100", "-w", "-DUM_HOME"]
    for include in (HERE / "shim", HERE, gfx, FIRMWARE / "clockfaces/home", FIRMWARE / "lib/faces", FIRMWARE / "lib/core", FIRMWARE / "lib/ha-feed"):
        command += ["-I", str(include)]
    command += [str(HERE / "home.cpp"), str(gfx / "Adafruit_GFX.cpp"), str(FIRMWARE / "clockfaces/home/Clockface.cpp"),
                str(FIRMWARE / "clockfaces/home/HomeRenderer.cpp"), str(FIRMWARE / "lib/ha-feed/HaDefaults.cpp"), "-o", str(binary)]
    subprocess.run(command, check=True)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=REPO / "docs/previews", help="where home-sheet.png goes")
    parser.add_argument("--size", type=int, default=256, help="pixels per screen on the sheet")
    parser.add_argument("--columns", type=int, default=5)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ultimatrix-home-") as temporary:
        work = Path(temporary)
        scenes = work / "scenes"
        scenes.mkdir()
        subprocess.run([str(build(work)), str(scenes)], check=True)
        names = [line.split("\t") for line in (scenes / "scenes.tsv").read_text().splitlines()]
        rows = -(-len(names) // args.columns)
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-pattern_type", "glob", "-i", str(scenes / "scene-*.ppm"), "-vf",
                        f"scale={args.size}:{args.size}:flags=neighbor,tile={args.columns}x{rows}:padding=8:color=0x303030",
                        "-frames:v", "1", str(args.out / "home-sheet.png")], check=True)
        for index, (_, caption) in enumerate(names):
            print(f"{index + 1:2}. {caption}")
    print("wrote", args.out / "home-sheet.png")


if __name__ == "__main__":
    main()
