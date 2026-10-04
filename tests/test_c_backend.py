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
        )
        for index, (code, source_arch, thumb, target_arch, expected) in enumerate(cases):
            with self.subTest(index=index), tempfile.TemporaryDirectory() as directory:
                program = lift(code, source_arch, thumb, target_arch=target_arch)
                source = Path(directory) / "translated.c"
                library = Path(directory) / "translated.so"
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
                translated = ctypes.CDLL(str(library)).radek_translated_entry
                translated.restype = ctypes.c_uint32
                self.assertEqual(translated(), expected)

    def test_c_backend_rejects_untrusted_names_and_non_leaf_operations(self):
        program = lift(struct.pack("<II", 0x52800500, 0xD65F03C0), "arm64")
        with self.assertRaisesRegex(Unsupported, "function name"):
            emit(program, "bad();system")

        from radek.ir import Block, Instruction, Op, Program

        unsupported = Program("arm64", [Block(0, [Instruction(Op.CALL, 0)])], b"", 4)
        with self.assertRaisesRegex(Unsupported, "no verified lowering"):
            emit(unsupported)
