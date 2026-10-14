#!/usr/bin/env python3
"""Render preview GIFs of the pixel-art faces (pacman, mario, luigi, word) with the real face code."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import showcase  # noqa: E402  (animate() turns frames into the GIF)

FIRMWARE = HERE.parent.parent
REPO = FIRMWARE.parent
FACES = ("pacman", "mario", "luigi", "word")


def render(face, work, extra):
    folder = FIRMWARE / "clockfaces" / face
    gfx = FIRMWARE / "vendor/gfx"
    output = work / face
    output.mkdir(exist_ok=True)
    command = [os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-DARDUINO=100", "-w", f"-DFACE_{face.upper()}"]
    if face in ("mario", "luigi"):
        command.append("-DFACE_NIGHT_AWARE")
    for include in (HERE / "shim", HERE, gfx, folder, FIRMWARE / "lib/faces", FIRMWARE / "lib/core"):
        command += ["-I", str(include)]
    command += [str(HERE / "faces.cpp"), str(gfx / "Adafruit_GFX.cpp")]
    command += [str(path) for path in sorted(folder.glob("*.cpp"))]
    command += extra + ["-o", str(work / f"faces-{face}")]
    subprocess.run(command, check=True)
    subprocess.run([str(work / f"faces-{face}"), str(output)], check=True, stdout=subprocess.DEVNULL)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--face", choices=FACES)
    parser.add_argument("--out", type=Path, default=REPO / "docs/previews")
    parser.add_argument("--work", type=Path, help="keep the native frames here")
    parser.add_argument("--size", type=int, default=400)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ultimatrix-faces-") as temporary:
        work = args.work or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        for face in [args.face] if args.face else FACES:
            frames = render(face, work, [])
            showcase.animate(frames, args.out / f"{face}.gif", args.size)


if __name__ == "__main__":
    main()
