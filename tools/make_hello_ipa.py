#!/usr/bin/env python3
"""Regenerate the committed hello-test IPA (tests/data/hello-test.ipa).

This is the simplest authorized IPA that is fully convertible by the bounded
``complete-game-v1`` backend:

* one ARM64 MH_EXECUTE slice, no dependencies, no imports, no metadata;
* the whole executable ``__text`` section is exactly one proven closed-integer
  routine (MOV immediate, ADD immediate, RET) returning 19;
* a ``__cstring`` section carries the launch message ``hello test succesfull``,
  which the converted APK recovers and displays when opened;
* no icon is declared, so host and device conversions never disagree about
  launcher icon bytes.

The host CLI converts it to a signed, installable ``complete-game-v1`` APK;
the Android app converts the same IPA on-device through the bounded converter.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from tests.fixtures import ipa, macho

LAUNCH_MESSAGE = "hello test succesfull"

# Returns 19 == len("hello test succesfull"), the message length.
CODE = struct.pack(
    "<III",
    0x52800240,  # MOV W0, #18
    0x11000000 | (1 << 10) | (0 << 5) | 0,  # ADD W0, W0, #1
    0xD65F03C0,  # RET -> returns 19
)

CSTRING = LAUNCH_MESSAGE.encode("ascii") + b"\x00"

OUTPUT = Path(__file__).resolve().parent.parent / "tests" / "data" / "hello-test.ipa"


def main() -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    executable = macho(CODE, cstring=CSTRING)
    ipa(
        OUTPUT,
        executable,
        icon=False,
        display_name="Hello Test",
        bundle_id="dev.radek.hellotest",
    )
    print(OUTPUT)


if __name__ == "__main__":
    main()
