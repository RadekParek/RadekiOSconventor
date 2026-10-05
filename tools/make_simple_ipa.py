#!/usr/bin/env python3
"""Regenerate the deterministic ``tests/data/simple.ipa`` stress fixture.

This is intentionally more involved than ``hello-test.ipa`` while remaining
inside the repository's current bounded converter contract: one ARM64 entry,
no imports/dependencies/relocations/runtime metadata, and a straight-line
sequence of supported integer operations. It is a converter test fixture, not
a game or a claim of general Angry Birds compatibility.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from tests.fixtures import ipa, macho

OUTPUT = Path(__file__).resolve().parent.parent / "tests" / "data" / "simple.ipa"
LAUNCH_MESSAGE = "Simple IPA: 128 integer operations statically recompiled"


def movz(register: int, immediate: int) -> int:
    return 0x52800000 | ((immediate & 0xFFFF) << 5) | register


def movk(register: int, immediate: int) -> int:
    return 0x72A00000 | ((immediate & 0xFFFF) << 5) | register


def mov(destination: int, source: int) -> int:
    return 0x2A0003E0 | (source << 16) | destination


def add(destination: int, source: int, immediate: int) -> int:
    return 0x11000000 | ((immediate & 0xFFF) << 10) | (source << 5) | destination


def sub(destination: int, source: int, immediate: int) -> int:
    return 0x51000000 | ((immediate & 0xFFF) << 10) | (source << 5) | destination


def build_code() -> tuple[bytes, int]:
    words = [movz(0, 0x1234)]
    accumulator = 0x1234

    # 128 deterministic ALU operations, including register copies and MOVK
    # updates. Every operation contributes to the returned accumulator. All
    # immediates fit AArch64's 12-bit ADD/SUB encoding.
    operation_count = 128
    for index in range(operation_count):
        immediate = (index * 73 + 19) % 2047 + 1
        operation = index % 4
        if operation == 0:
            words.append(add(0, 0, immediate))
            accumulator = (accumulator + immediate) & 0xFFFFFFFF
        elif operation == 1:
            words.append(sub(0, 0, immediate))
            accumulator = (accumulator - immediate) & 0xFFFFFFFF
        elif operation == 2:
            words.extend((mov(1, 0), add(1, 1, immediate), mov(0, 1)))
            accumulator = (accumulator + immediate) & 0xFFFFFFFF
        else:
            words.extend((mov(2, 0), add(2, 2, immediate), mov(0, 2)))
            accumulator = (accumulator + immediate) & 0xFFFFFFFF

        if index % 16 == 15:
            replacement = (index * 313 + 0x4B21) & 0x3FFF
            words.append(movk(0, replacement))
            accumulator = (accumulator & 0x0000FFFF) | (replacement << 16)

    # Finish with two more checked immediate operations, then return the result.
    words.extend((add(0, 0, 29), sub(0, 0, 7), 0xD65F03C0))
    result = (accumulator + 29 - 7) & 0xFFFFFFFF
    return struct.pack("<" + "I" * len(words), *words), result


CODE, RETURN_VALUE = build_code()
CSTRING = LAUNCH_MESSAGE.encode("ascii") + b"\x00"


def main() -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    executable = macho(CODE, cstring=CSTRING)
    ipa(
        OUTPUT,
        executable,
        icon=False,
        display_name="Simple IPA",
        bundle_id="dev.radek.simpleipa",
    )
    print(f"{OUTPUT} ({len(CODE)} native entry bytes, return {RETURN_VALUE})")


if __name__ == "__main__":
    main()
