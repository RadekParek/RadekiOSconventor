import ctypes
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from radek.c_backend import emit
from radek.ir import Unsupported, lift


class CBackendTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which(os.environ.get("CXX", "g++")), "C++ compiler unavailable")
    def test_generated_c_executes_same_closed_leaf_result(self):
        cases = (
            (
                struct.pack("<III", 0x52800500, 0x11000800, 0xD65F03C0),
                "arm64",
                False,
                "arm64",
                42,
            ),
            (
                struct.pack("<IIII", 0x529FFFE0, 0x72BFFFE0, 0x11000400, 0xD65F03C0),
                "arm64",
                False,
                "arm64",
                0,
            ),
            (
                struct.pack("<HH", 0x202A, 0x4770),
                "armv6",
                True,
                "armv7",
                42,
            ),
            (
                # MOV W0,#40 ; MOV W1,W0 ; MOV X2,X1 ; ADD W2,W2,#2 ; MOV W0,W2 ; RET
                struct.pack(
                    "<IIIIII", 0x52800500, 0x2A0003E1, 0xAA0103E2, 0x11000842, 0x2A0203E0, 0xD65F03C0
                ),
                "arm64",
                False,
                "arm64",
                42,
            ),
            (
                # Thumb: MOVS r0,#41 ; MOV r1,r0 ; ADDS r1,#1 (via MOV chain) ; MOV r0,r1 ; BX LR
                struct.pack("<HHHHHH", 0x2029, 0x4601, 0x3101, 0x4608, 0x4684, 0x4770),
                "armv7",
                True,
                "arm64",
                42,
            ),
        )
        for index, (code, source_arch, thumb, target_arch, expected) in enumerate(cases):
            with self.subTest(index=index), tempfile.TemporaryDirectory() as directory:
                program = lift(code, source_arch, thumb, target_arch=target_arch)
                source = Path(directory) / "recompiled.c"
                library = Path(directory) / "recompiled.so"
                source.write_text(emit(program), encoding="utf-8")
                subprocess.run(
                    [
                        os.environ.get("CXX", "g++"),
                        "-std=c++17",
                        "-x",
                        "c++",
                        "-shared",
                        "-fPIC",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        str(source),
                        "-o",
                        str(library),
                    ],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                native_entry = ctypes.CDLL(str(library)).radek_recompiled_entry
                native_entry.restype = ctypes.c_uint32
                self.assertEqual(native_entry(), expected)

    def test_c_backend_rejects_untrusted_names_and_non_leaf_operations(self):
        program = lift(struct.pack("<II", 0x52800500, 0xD65F03C0), "arm64")
        with self.assertRaisesRegex(Unsupported, "function name"):
            emit(program, "bad();system")

        from radek.ir import Block, Instruction, Op, Program

        unsupported = Program("arm64", [Block(0, [Instruction(Op.CALL, 0)])], b"", 4)
        with self.assertRaisesRegex(Unsupported, "no verified lowering"):
            emit(unsupported)
