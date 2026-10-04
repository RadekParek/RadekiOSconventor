import hashlib
import struct
import unittest

from radek.archive import InputError
from radek.elf import inspect
from radek.elf_writer import build_shared_object
from radek.ir import lift


class ELFWriterTests(unittest.TestCase):
    def test_generated_arm64_dso_exports_exact_translated_machine_code(self):
        source = struct.pack("<II", 0x52800540, 0xD65F03C0)
        program = lift(source, "arm64")
        image = build_shared_object(program.machine_code, "arm64")
        info = inspect(image)

        self.assertEqual(info["architecture"], "arm64-v8a")
        self.assertEqual(info["needed"], [])
        self.assertEqual(info["undefinedSymbols"], [])
        exported = info["exports"]["radek_translated_entry"]
        self.assertEqual(exported["size"], len(program.machine_code))
        self.assertEqual(exported["sha256"], hashlib.sha256(program.machine_code).hexdigest())

    def test_generated_armv7_dso_has_arm32_machine_and_export(self):
        source = struct.pack("<HH", 0x202A, 0x4770)
        program = lift(source, "armv6", True, target_arch="armv7")
        image = build_shared_object(program.machine_code, "armv7")
        info = inspect(image)

        self.assertEqual(info["architecture"], "armeabi-v7a")
        self.assertEqual(info["needed"], [])
        self.assertEqual(info["undefinedSymbols"], [])
        self.assertEqual(info["exports"]["radek_translated_entry"]["size"], len(program.machine_code))

    def test_rejects_empty_code_bad_symbol_and_unsupported_architecture(self):
        for args in ((b"", "arm64"), (b"\xc0\x03\x5f\xd6", "x86_64"),
                     (b"\xc0\x03\x5f\xd6", "arm64", "bad;symbol")):
            with self.subTest(args=args), self.assertRaises(InputError):
                build_shared_object(*args)
