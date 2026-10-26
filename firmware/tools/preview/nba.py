#!/usr/bin/env python3
"""Render the NBA face's pages (live game, quarters, today, upcoming, standings, clock, basket) with the real drawing code and made-up games.

Writes a contact sheet; needs a C++ compiler and ffmpeg (no Pillow)."""
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
    binary = work / "nba-preview"
    command = [os.environ.get("CXX", "c++"), "-std=c++17", "-O1", "-DARDUINO=100", "-w", "-DUM_NBA"]
    for include in (HERE / "shim", HERE, gfx, FIRMWARE / "clockfaces/nba", FIRMWARE / "lib/faces", FIRMWARE / "lib/core", FIRMWARE / "lib/nba-feed"):
        command += ["-I", str(include)]
    command += [str(HERE / "nba.cpp"), str(gfx / "Adafruit_GFX.cpp"), str(FIRMWARE / "clockfaces/nba/NbaRenderer.cpp"), "-o", str(binary)]
    subprocess.run(command, check=True)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=REPO / "docs/previews", help="where nba-sheet.png goes")
    parser.add_argument("--size", type=int, default=256, help="pixels per screen on the sheet")
    parser.add_argument("--columns", type=int, default=6)
    parser.add_argument("--keep", type=Path, help="also keep the scene PPMs in this folder")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ultimatrix-nba-") as temporary:
        work = Path(temporary)
        scenes = work / "scenes"
        scenes.mkdir()
        subprocess.run([str(build(work)), str(scenes)], check=True)
        names = [line.split("\t") for line in (scenes / "scenes.tsv").read_text().splitlines()]
        rows = -(-len(names) // args.columns)
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-pattern_type", "glob", "-i", str(scenes / "scene-*.ppm"), "-vf",
                        f"scale={args.size}:{args.size}:flags=neighbor,tile={args.columns}x{rows}:padding=8:color=0x303030",
                        "-frames:v", "1", str(args.out / "nba-sheet.png")], check=True)
        if args.keep:
            args.keep.mkdir(parents=True, exist_ok=True)
            for ppm in scenes.glob("scene-*.ppm"):
                (args.keep / ppm.name).write_bytes(ppm.read_bytes())
        for index, (_, caption) in enumerate(names):
            print(f"{index + 1:2}. {caption}")
    print("wrote", args.out / "nba-sheet.png")


if __name__ == "__main__":
    main()
