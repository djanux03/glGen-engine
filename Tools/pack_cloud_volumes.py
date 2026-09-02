#!/usr/bin/env python3
"""Pack a sequence of TGA slices into a single 3D-texture blob (.glvol).

The volumetric cloud system needs two 128^3 RGBA volumes -- the base shape
noise and the Nubis detail noise. Both ship upstream as one TGA per Z slice
(128 files each), which is ~48 MB of RLE TGA and 128 image decodes at every
startup. This bakes them once into a flat RGBA8 blob the renderer uploads
with a single fread + one vkCmdCopyBufferToImage.

Format (little-endian):
    char[8]  "GLVOL1\0\0"
    uint32   width, height, depth, channels   (channels is always 4)
    uint8[]  width*height*depth*4, slice-major (z outer, then y, then x)

Run from anywhere; paths are arguments. See the __main__ block for the two
invocations that produced assets/clouds/*.glvol.
"""

import argparse
import os
import struct
import sys

from PIL import Image

MAGIC = b"GLVOL1\0\0"


def pack(pattern, depth, out_path):
    """pattern is a printf-style format taking the slice index, e.g.
    ".../lowResCloud(%d).tga" -- upstream numbers slices from 0."""
    data = bytearray()
    width = height = None
    for z in range(depth):
        path = pattern % z
        with Image.open(path) as im:
            im = im.convert("RGBA")
            if width is None:
                width, height = im.size
            elif im.size != (width, height):
                raise SystemExit(
                    "slice %d is %dx%d, expected %dx%d" % (z, *im.size, width, height)
                )
            data += im.tobytes()

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<4I", width, height, depth, 4))
        f.write(data)
    print(
        "%s  %dx%dx%d RGBA8  (%.1f MB)"
        % (out_path, width, height, depth, len(data) / (1024.0 * 1024.0))
    )


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("pattern", help="printf pattern for slice paths, %d = slice index")
    ap.add_argument("depth", type=int, help="number of slices")
    ap.add_argument("out", help="output .glvol path")
    args = ap.parse_args(argv)
    pack(args.pattern, args.depth, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
