#!/usr/bin/env python3
"""Read-only identity probe for the optional persistent converter cache.

Run on the iPad as mobile (uid 501), with every virtual machine shut down. The
GPU task verifies the receipt as mobile, and st_gen reads differently for root,
so a capture taken as root would never match. Prints one JSON object.
"""
import ctypes
import hashlib
import json
import os
import sys

DIRECTORY = "/var/root/VirtualMac2/compiler27"
FILES = (
    "VM27CompilerCompat.dylib", "VM27GPUCompiler.dylib", "VM27GPUCompilerImpl.dylib",
    "VM27LLVM.dylib", "VM27llvm-flatbuffers.dylib", "VM27llvm-lmdb.dylib",
    "metal-library-convert",
)
PROFILE = "compiler-profile.json"


class Timespec(ctypes.Structure):
    _fields_ = [("sec", ctypes.c_int64), ("nsec", ctypes.c_int64)]


class Stat(ctypes.Structure):
    # Darwin arm64 struct stat with 64-bit inodes.
    _fields_ = [
        ("dev", ctypes.c_int32), ("mode", ctypes.c_uint16), ("nlink", ctypes.c_uint16),
        ("ino", ctypes.c_uint64), ("uid", ctypes.c_uint32), ("gid", ctypes.c_uint32),
        ("rdev", ctypes.c_int32), ("atime", Timespec), ("mtime", Timespec),
        ("ctime", Timespec), ("birth", Timespec), ("size", ctypes.c_int64),
        ("blocks", ctypes.c_int64), ("blksize", ctypes.c_int32), ("flags", ctypes.c_uint32),
        ("gen", ctypes.c_uint32), ("lspare", ctypes.c_int32), ("qspare", ctypes.c_int64 * 2),
    ]


LIBC = ctypes.CDLL(None, use_errno=True)


def capture(descriptor):
    value = Stat()
    if LIBC.fstat(descriptor, ctypes.byref(value)):
        raise OSError(ctypes.get_errno(), "fstat")
    # The hand-written layout must agree with the interpreter's own view.
    check = os.fstat(descriptor)
    if (value.ino, value.size, value.dev, value.mode, value.mtime.sec * 10**9 + value.mtime.nsec,
            value.flags, value.gen) != (check.st_ino, check.st_size, check.st_dev, check.st_mode,
                                        check.st_mtime_ns, check.st_flags, check.st_gen):
        raise SystemExit("struct stat layout does not match this system")
    return {
        "dev": value.dev, "ino": value.ino, "size": value.size, "uid": value.uid,
        "gid": value.gid, "nlink": value.nlink, "mode": value.mode, "flags": value.flags,
        "gen": value.gen, "rdev": value.rdev, "blocks": value.blocks, "blksize": value.blksize,
        "mtimeSec": value.mtime.sec, "mtimeNsec": value.mtime.nsec,
        "ctimeSec": value.ctime.sec, "ctimeNsec": value.ctime.nsec,
        "birthSec": value.birth.sec, "birthNsec": value.birth.nsec,
    }


def read(name):
    descriptor = os.open(os.path.join(DIRECTORY, name), os.O_RDONLY | os.O_NOFOLLOW)
    try:
        before = capture(descriptor)
        digest = hashlib.sha256()
        while True:
            chunk = os.read(descriptor, 1 << 20)
            if not chunk:
                break
            digest.update(chunk)
        after = capture(descriptor)
    finally:
        os.close(descriptor)
    if before != after:
        raise SystemExit(name + " changed while it was read")
    return {"filename": name, "sha256": digest.hexdigest(), "stat": before}


def main():
    if os.getuid() != 501 or os.geteuid() != 501:
        raise SystemExit("run as mobile (uid 501), not root")
    result = {"uid": os.getuid(), "directory": DIRECTORY,
              "profile": read(PROFILE), "files": [read(name) for name in FILES]}
    json.dump(result, sys.stdout, sort_keys=True, separators=(",", ":"))
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
