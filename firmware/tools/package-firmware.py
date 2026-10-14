#!/usr/bin/env python3
"""Package current-mode application images and signed manifests for the clock's server."""
import argparse
import hashlib
import json
import time
from pathlib import Path
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path, default=root / 'dist/firmware')
parser.add_argument('--flat', action='store_true', help='GitHub release layout: <mode>-<file> in one directory')
args = parser.parse_args()
key = serialization.load_pem_private_key((root / '.tools/update-signing.pem').read_bytes(), password=None)
public = key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
for mode in ('football', 'f1', 'motogp', 'tetris', 'word', 'mario', 'luigi', 'pacman'):
    if not (root / 'firmware/build' / mode / 'ultimatrix.bin').is_file():
        print(f'{mode}: not built, skipped')
        continue
    image = (root / 'firmware/build' / mode / 'ultimatrix.bin').read_bytes()
    if len(image) < 288 or image[0] != 0xE9 or public not in image:
        raise SystemExit(f'{mode}: rebuild with the matching update key before packaging')
    version = image[48:80].split(bytes([0]), 1)[0].decode()
    project = image[80:112].split(bytes([0]), 1)[0].decode()
    if project != 'ultimatrix': raise SystemExit(f'{mode}: not an Ultimatrix application image')
    digest = hashlib.sha256(image).hexdigest()
    name = f'ultimatrix-{digest}.bin'
    manifest = {'schema':1, 'release':int(time.time()), 'mode':mode, 'version':version, 'image':name, 'size':len(image),
                'sha256':digest, 'elfSha256':image[176:208].hex()}
    data = (json.dumps(manifest, separators=(',', ':'), sort_keys=True) + '\n').encode()
    signature = key.sign(data, ec.ECDSA(hashes.SHA256()))
    target = args.out if args.flat else args.out / mode; target.mkdir(parents=True, exist_ok=True)
    prefix = f'{mode}-' if args.flat else ''
    (target / f'{prefix}{name}').write_bytes(image)
    (target / f'{prefix}manifest.json').write_bytes(data)
    (target / f'{prefix}manifest.sig').write_bytes(signature)
    print(f'{mode}: packaged {version}, {len(image)} bytes')
