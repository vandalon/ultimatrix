#!/usr/bin/env python3
"""Writes lib/wled/klipy_key.h (gitignored) from a KLIPY key read at a hidden prompt (or the KLIPY_KEY environment variable, as CI sets it), never from the command line.
Obfuscation only (XOR with a mask): it keeps the key out of plain text searches and does not protect it."""
import getpass, os, pathlib
MASK = b"ultimatrix"  # same mask as in WledCanvas.cpp
key = (os.environ.get("KLIPY_KEY") or getpass.getpass("KLIPY API key: ")).strip().encode()
out = pathlib.Path(__file__).resolve().parent.parent / "lib/wled/klipy_key.h"
out.write_text('// Local only (gitignored): the KLIPY key XORed with "ultimatrix". Make it with tools/obfuscate-key.py\n#pragma once\n'
               "static const unsigned char kKlipyKey[] = {%s};\n" % ",".join(str(b ^ MASK[i % len(MASK)]) for i, b in enumerate(key)))
print("Wrote", out)
