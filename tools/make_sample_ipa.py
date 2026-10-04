#!/usr/bin/env python3
"""Regenerate the committed synthetic sample IPA (tests/data/sample-leaf.ipa).

The sample is a small authorized leaf program (returns 42) using the proven
straight-line integer subset, plus a handful of Darwin imports that exercise
the compatibility-registry split (verified time shim vs explicit stubs).
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from tests.fixtures import ipa, macho

CODE = struct.pack(
    "<IIIII",
    0x52800500,  # MOV W0, #40
    0x2A0003E1,  # MOV W1, W0   (register copy)
    0x11000821,  # ADD W1, W1, #2
    0x2A0103E0,  # MOV W0, W1   (register copy)
    0xD65F03C0,  # RET          -> returns 42
)

IMPORTS = (
    "_CFAbsoluteTimeGetCurrent",
    "_glDrawArrays",
    "_OBJC_CLASS_$_UIView",
)

OUTPUT = Path(__file__).resolve().parent.parent / "tests" / "data" / "sample-leaf.ipa"


def main() -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    ipa(OUTPUT, macho(CODE, imports=IMPORTS))
    print(OUTPUT)


if __name__ == "__main__":
    main()
