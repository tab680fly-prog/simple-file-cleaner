#!/usr/bin/env python3
"""Builds a multi-resolution .ico from an SVG using rsvg-convert.

Usage: make_ico.py <input.svg> <output.ico>
"""
import struct
import subprocess
import sys

SIZES = [16, 24, 32, 48, 64, 128, 256]


def main():
    svg, out = sys.argv[1], sys.argv[2]
    images = [subprocess.run(["rsvg-convert", "-w", str(s), "-h", str(s), svg], check=True,
                             capture_output=True).stdout for s in SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, data = b"", b""
    for size, png in zip(SIZES, images):
        dim = 0 if size >= 256 else size  # 0 means 256 in the ICO format
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(png), offset + len(data))
        data += png
    with open(out, "wb") as f:
        f.write(header + entries + data)


if __name__ == "__main__":
    main()
