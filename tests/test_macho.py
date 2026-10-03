import json
import os
import random
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from radek.analysis import analyze, analyzer_path
from radek.archive import InputError
from .fixtures import macho, fat


class MachOTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "exe"

    def parse(self, data):
        self.path.write_bytes(data)
        return analyze(self.path)

    def test_arm64_sections_symbols_entry(self):
        s = self.parse(macho())["slices"][0]
        self.assertEqual(s["architecture"], "arm64")
        self.assertEqual(s["entryOffset"], 4096)
        self.assertEqual(s["segments"][0]["sections"][0]["name"], "__text")
        self.assertEqual(s["symbols"][0]["name"], "_main")
        self.assertEqual(s["exports"][0]["name"], "_main")

    def test_fat_and_fat64_both_endians(self):
        for wide in (True, False):
            for little in (True, False):
                with self.subTest(wide=wide, little=little):
                    result = self.parse(fat([macho(), macho(cpu=12, subtype=9)], wide, little))
                    self.assertEqual([s["architecture"] for s in result["slices"]], ["arm64", "armv7"])

    def test_arm64e(self):
        self.assertTrue(self.parse(macho(subtype=2))["slices"][0]["pacRequired"])

    def test_armv7s(self):
        self.assertEqual(self.parse(macho(cpu=12, subtype=11))["slices"][0]["architecture"], "armv7s")

    def test_armv6(self):
        self.assertEqual(self.parse(macho(cpu=12, subtype=6))["slices"][0]["architecture"], "armv6")

    def test_encrypted(self):
        self.assertTrue(self.parse(macho(encrypted=True))["slices"][0]["encrypted"])

    def test_imports_dependencies(self):
        s = self.parse(macho(imports=["_malloc"], dependencies=["/usr/lib/libSystem.B.dylib"]))["slices"][0]
        self.assertEqual(s["imports"][0]["name"], "_malloc")
        self.assertEqual(s["dependencies"][0]["path"], "/usr/lib/libSystem.B.dylib")

    def test_relocations(self):
        r = self.parse(macho(reloc=True))["slices"][0]["segments"][0]["sections"][0]["relocations"][0]
        self.assertTrue(r["external"])
        self.assertEqual(r["type"], 1)

    def test_metadata_discovery(self):
        for name in ("__objc_classlist", "__swift5_types", "__unwind_info", "__eh_frame"):
            self.assertEqual(
                self.parse(macho(section_name=name))["slices"][0]["metadata"][0]["section"], name
            )

    def test_export_trie(self):
        trie = b"\x00\x01_foo\x00\x08\x02\x00\x2a\x00"
        cmd = struct.pack("<IIII", 0x80000033, 16, 0x2000, len(trie))
        s = self.parse(macho(extras=[cmd], blobs={0x2000: trie}))["slices"][0]
        self.assertEqual(s["exports"][-1]["name"], "_foo")
        self.assertEqual(s["exports"][-1]["address"], 42)

    def test_cyclic_export_trie_rejected(self):
        trie = b"\x00\x01a\x00\x00"
        with self.assertRaises(InputError):
            self.parse(
                macho(extras=[struct.pack("<IIII", 0x80000033, 16, 0x2000, len(trie))], blobs={0x2000: trie})
            )

    def test_bind_symbols(self):
        bind = b"\x11\x40_malloc\x00\x70\x00\x90\x00"
        cmd = struct.pack("<12I", 0x80000022, 48, 0, 0, 0x2000, len(bind), 0, 0, 0, 0, 0, 0)
        s = self.parse(macho(extras=[cmd], blobs={0x2000: bind}))["slices"][0]
        self.assertEqual(s["imports"][0]["name"], "_malloc")
        self.assertEqual(s["imports"][0]["ordinal"], 1)

    def test_chained_fixup_imports(self):
        blob = (
            struct.pack("<7I", 0, 28, 32, 36, 1, 1, 0)
            + struct.pack("<I", 0)
            + struct.pack("<I", 1)
            + b"_malloc\x00"
        )
        cmd = struct.pack("<IIII", 0x80000034, 16, 0x2000, len(blob))
        s = self.parse(macho(extras=[cmd], blobs={0x2000: blob}))["slices"][0]
        self.assertEqual(s["chainedFixups"]["imports"][0]["name"], "_malloc")

    def test_signature_metadata(self):
        identifier = b"org.test\x00"
        directory = (
            struct.pack(
                ">9I4BI", 0xFADE0C02, 44 + len(identifier), 0x20001, 2, 44, 44, 0, 0, 0, 32, 2, 0, 12, 0
            )
            + identifier
        )
        blob = struct.pack(">5I", 0xFADE0CC0, 20 + len(directory), 1, 0, 20) + directory
        cmd = struct.pack("<IIII", 0x1D, 16, 0x2000, len(blob))
        s = self.parse(macho(extras=[cmd], blobs={0x2000: blob}))["slices"][0]
        self.assertEqual(s["codeSignature"]["blobs"][0]["identifier"], "org.test")
        self.assertEqual(s["codeSignature"]["cryptographicVerification"], "not-performed")

    def test_dynamic_symbols(self):
        cmd = struct.pack("<20I", 0xB, 80, *([0] * 18))
        self.assertIn("dynamicSymbols", self.parse(macho(extras=[cmd]))["slices"][0])

    def test_fat_overlap_rejected(self):
        data = bytearray(fat([macho(), macho()]))
        struct.pack_into(">I", data, 8 + 20 + 8, 4096)
        with self.assertRaises(InputError):
            self.parse(data)

    def test_truncated_commands(self):
        data = bytearray(macho())
        struct.pack_into("<I", data, 36, 7)
        with self.assertRaises(InputError):
            self.parse(data)

    def test_truncated_header(self):
        with self.assertRaises(InputError):
            self.parse(b"\xcf\xfa\xed\xfe")

    def test_random_malformed_inputs_never_crash(self):
        rng = random.Random(42)
        for _ in range(60):
            data = bytearray(macho())
            for _ in range(rng.randrange(1, 15)):
                data[rng.randrange(256)] = rng.randrange(256)
            self.path.write_bytes(data)
            p = subprocess.run([str(analyzer_path()), str(self.path)], capture_output=True, timeout=3)
            self.assertIn(p.returncode, (0, 1), p.stderr)
            if not p.returncode:
                json.loads(p.stdout)

    def test_bind_repeat_scaled_skip_and_signed_addend(self):
        bind = b"\x31\x40_sym\x00\x70\x00\x60\x7f\xb2\xc0\x02\x08\x00"
        cmd = struct.pack("<12I", 0x80000022, 48, 0, 0, 0x2000, len(bind), 0, 0, 0, 0, 0, 0)
        slice_data = self.parse(macho(extras=[cmd], blobs={0x2000: bind}))["slices"][0]
        imports = slice_data["imports"]
        self.assertEqual([i["offset"] for i in imports], [0, 24, 40])
        self.assertTrue(all(i["addend"] == -1 and i["ordinal"] == -15 for i in imports))
        self.assertTrue(slice_data["bindDecodingComplete"])

    def test_bind_address_overflow_is_reported_without_aborting_analysis(self):
        bind = b"\x40_symbol\x00\x70" + b"\xff" * 9 + b"\x01\x80\x01\x00"
        cmd = struct.pack("<12I", 0x80000022, 48, 0, 0, 0x2000, len(bind), 0, 0, 0, 0, 0, 0)
        slice_data = self.parse(macho(extras=[cmd], blobs={0x2000: bind}))["slices"][0]
        self.assertFalse(slice_data["bindDecodingComplete"])
        self.assertIn("outside segment", slice_data["bindDiagnostics"][0]["message"])
        self.assertEqual(slice_data["imports"], [])

    def test_section_outside_segment_rejected(self):
        data = bytearray(macho())
        struct.pack_into("<Q", data, 32 + 72 + 32, 1)
        with self.assertRaises(InputError):
            self.parse(data)

    def test_excessively_long_symbol_rejected(self):
        with self.assertRaises(InputError):
            self.parse(macho(imports=["x" * 4097]))

    def test_unixthread_pc_sp(self):
        state = bytearray(272)
        struct.pack_into("<Q", state, 256, 0x100001000)
        struct.pack_into("<Q", state, 248, 0x777000)
        command = struct.pack("<4I", 5, 16 + len(state), 6, 68) + state
        entry = self.parse(macho(extras=[command]))["slices"][0]["threadEntry"]
        self.assertEqual(entry["programCounter"], 0x100001000)
        self.assertEqual(entry["stackPointer"], 0x777000)

    def test_chained_page_starts(self):
        starts = struct.pack("<II", 1, 8) + struct.pack("<IHHQIHH", 24, 4096, 2, 0, 0, 1, 0xFFFF)
        blob = struct.pack("<7I", 0, 28, 60, 60, 0, 1, 0) + starts
        cmd = struct.pack("<4I", 0x80000034, 16, 0x2000, len(blob))
        chain = self.parse(macho(extras=[cmd], blobs={0x2000: blob}))["slices"][0]["chainedFixups"]
        self.assertEqual(chain["segments"][0]["pointerFormat"], 2)
        self.assertEqual(chain["segments"][0]["pageStarts"], [65535])
        self.assertEqual(chain["pointerTraversal"], "not-implemented")
