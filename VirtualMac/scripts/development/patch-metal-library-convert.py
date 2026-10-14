#!/usr/bin/env python3
"""Let the reviewed converter helper accept Mac Catalyst shader libraries.

The helper source is unavailable: this is a guarded binary transformation of
the exact 2.0.0beta1 metal-library-convert. After Apple's downgrade pass it
compares the module triple with air64-apple-macosx13.0.0 and exits otherwise,
which rejects every air64-apple-ios*-macabi library a Catalyst app compiles.
Both comparisons become unconditional; modern_pvg_shader_worker.m keeps the
list of accepted output targets. The result is unsigned.
"""
import argparse
import hashlib
from pathlib import Path
import struct

INPUT_SHA256 = "92521f90f368cb7eb9c0d3b5abf3c3cf788f478954a3372651124e8db3a94311"
# (file offset, cbz w0 to the accepting block, b to the same block)
SITES = ((0x62e8, 0x34000200, 0x14000010), (0x7020, 0x340000c0, 0x14000006))
STRCMP_CALL_MASK, STRCMP_CALL = 0xfc000000, 0x94000000


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    data = bytearray(arguments.input.read_bytes())
    if hashlib.sha256(data).hexdigest() != INPUT_SHA256:
        raise SystemExit("error: not the reviewed 2.0.0beta1 metal-library-convert")
    for offset, original, replacement in SITES:
        call, branch = struct.unpack_from("<II", data, offset - 4)
        if call & STRCMP_CALL_MASK != STRCMP_CALL or branch != original:
            raise SystemExit(f"error: unexpected instructions at {offset:#x}")
        struct.pack_into("<I", data, offset, replacement)
    with open(arguments.output, "xb") as output:
        output.write(data)
    arguments.output.chmod(0o755)
    print(hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    main()
