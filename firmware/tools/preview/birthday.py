#!/usr/bin/env python3
"""Render the birthday screens natively: birthday.py [party] [seconds] [--name N] [--age A] [--time HH:MM]."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import showcase  # noqa: E402

FIRMWARE = HERE.parent.parent
REPO = FIRMWARE.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", choices=("party",), nargs="?", default="party")
    parser.add_argument("seconds", nargs="?", type=int, default=24)
    parser.add_argument("--name", default="ELIOT")
    parser.add_argument("--age", default="7")
    parser.add_argument("--time", default="17:42")
    parser.add_argument("--size", type=int, default=400)
    parser.add_argument("--out", type=Path, default=REPO / "docs/previews")
    parser.add_argument("--frames", type=Path, help="keep the native frames here")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ultimatrix-birthday-") as temporary:
        work = args.frames or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        binary = work / "birthday"
        command = [os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-DARDUINO=100", "-w",
                   "-I", str(HERE / "shim"), "-I", str(FIRMWARE / "lib/core"), str(HERE / "birthday.cpp"), "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary), str(work), args.scene, str(args.seconds), args.name, args.age, args.time], check=True)
        showcase.animate(work, args.out / f"birthday-{args.scene}.gif", args.size)


if __name__ == "__main__":
    main()
