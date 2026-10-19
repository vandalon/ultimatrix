#!/usr/bin/env python3
"""Compress one web asset for embedding: gzip -9 with a fixed timestamp so builds are reproducible."""
import gzip
import sys

source, target = sys.argv[1], sys.argv[2]
with open(source, "rb") as f:
    data = f.read()
with open(target, "wb") as out:
    with gzip.GzipFile(filename="", mode="wb", compresslevel=9, fileobj=out, mtime=0) as z:
        z.write(data)
