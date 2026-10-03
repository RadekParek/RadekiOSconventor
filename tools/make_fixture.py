#!/usr/bin/env python3
import argparse
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from tests.fixtures import ipa, macho

p = argparse.ArgumentParser(description="Create an authorized synthetic leaf-program IPA returning 42")
p.add_argument("--output", required=True, type=Path)
p.add_argument("--arch", choices=["arm64", "arm64e", "armv7", "armv7s", "armv6", "thumb", "thumb2"], default="arm64")
p.add_argument("--xml", action="store_true")
a = p.parse_args()
a.output.parent.mkdir(parents=True, exist_ok=True)
if a.arch in ("arm64", "arm64e"):
    executable = macho(subtype=2 if a.arch == "arm64e" else 0)
elif a.arch in ("thumb", "thumb2"):
    code = (
        struct.pack("<HH", 0x202A, 0x4770)
        if a.arch == "thumb"
        else struct.pack("<HHH", 0xF240, 0x002A, 0x4770)
    )
    executable = macho(code, cpu=12, subtype=9, thumb=True)
else:
    subtype = {"armv7s": 11, "armv7": 9, "armv6": 6}[a.arch]
    executable = macho(cpu=12, subtype=subtype)
ipa(a.output, executable, binary=not a.xml)
print(a.output)
