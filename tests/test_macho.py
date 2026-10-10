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


def _uleb(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


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

    def _bind(self, stream):
        cmd = struct.pack("<12I", 0x80000022, 48, 0, 0, 0x2000, len(stream), 0, 0, 0, 0, 0, 0)
        return self.parse(macho(extras=[cmd], blobs={0x2000: stream}))["slices"][0]

    def test_dyld_bind_address_overflow_is_reported_not_fatal(self):
        """A overflowing bind address used to abort the whole import with an
        IOException. The stream is now stopped, the reason recorded, and the rest
        of the slice is still analyzed."""
        stream = b"\x40_sym\x00\x70\x00\x90\x80" + b"\xff" * 9 + b"\x01"
        s = self._bind(stream)
        self.assertEqual(s["imports"][0]["name"], "_sym")
        self.assertEqual(s["architecture"], "arm64")
        self.assertEqual(s["fixupStreams"][0]["status"], "partial")
        self.assertEqual(s["fixupStreams"][0]["decodedBinds"], 1)
        reasons = [a["reason"] for a in s["fixupAnomalies"]]
        # The merged decoder bounds the cursor against the segment size, so the
        # reported reason names the segment bound. What matters is that the stream
        # is reported as partial instead of aborting the analysis.
        self.assertTrue(any("dyld bind" in reason for reason in reasons), reasons)
        self.assertEqual(s["fixupAnomalies"][0]["stream"], "bind")

    def test_dyld_bind_outside_segment_is_reported_not_fatal(self):
        stream = b"\x40_sym\x00\x70\xff\xff\x7f\x90"
        s = self._bind(stream)
        self.assertEqual(s["imports"], [])
        self.assertEqual(s["fixupStreams"][0]["status"], "partial")
        self.assertTrue(
            any("dyld bind" in a["reason"] for a in s["fixupAnomalies"]),
            [a["reason"] for a in s["fixupAnomalies"]],
        )

    def test_truncated_bind_stream_is_reported_not_fatal(self):
        s = self._bind(b"\x40_sym\x00\x80\xff\xff")
        self.assertEqual(s["fixupStreams"][0]["status"], "partial")
        self.assertIn("truncated ULEB128", [a["reason"] for a in s["fixupAnomalies"]])

    def test_threaded_bind_ordinals_decoded(self):
        s = self._bind(b"\xd0\x00\x07\xd1")
        self.assertEqual(s["fixupStreams"][0]["status"], "decoded")
        self.assertEqual(s["fixupStreams"][0]["threadedOrdinals"], 1)
        self.assertEqual(s["fixupAnomalies"], [])

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

    def test_large_valid_bind_table_decodes_completely(self):
        # Real games routinely carry hundreds of thousands of bind opcodes across
        # the bind/weak/lazy streams. The complexity guard must scale with the
        # actual input instead of aborting a perfectly valid table mid-stream
        # (which used to report "Mach-O analysis complexity limit" and flip
        # bindDecodingComplete=false).
        bind = bytearray()
        for i in range(600000):
            sym = ("_s%05d" % i).encode() + b"\x00"
            bind += b"\x40" + sym + b"\x11\x70" + _uleb(0x100 + i * 4) + b"\x90"
        bind += b"\x00"
        cmd = struct.pack("<12I", 0x80000022, 48, 0, 0, 0x2000, len(bind), 0, 0, 0, 0, 0, 0)
        slice_data = self.parse(macho(cpu=12, subtype=9, vmsize=0x400000, extras=[cmd], blobs={0x2000: bytes(bind)}))["slices"][0]
        self.assertTrue(slice_data["bindDecodingComplete"])
        self.assertEqual(slice_data["bindDiagnostics"], [])
        stream = [s for s in slice_data["fixupStreams"] if s["kind"] == "bind"][0]
        self.assertEqual(stream["status"], "decoded")
        self.assertEqual(stream["decodedBinds"], 600000)

    def test_weak_lazy_and_threaded_streams_decode(self):
        weak = b"\x40_w1\x00\x11\x70\x40\x90\x00"
        lazy = b"\x40_l1\x00\x11\x70\x48\x90\x00\x40_l2\x00\x11\x70\x50\x90\x00"
        # Threaded records: SET_SYMBOL, SET_TYPE, THREADED|SET_ORDINAL(0) + ULEB,
        # THREADED|APPLY(1) = 0xd1.
        threaded = b"\x40_t1\x00\x51\xd0\x05\xd1" + b"\x40_t2\x00\x51\xd0\x09\xd1" + b"\x00"
        cmd = struct.pack(
            "<12I", 0x80000022, 48, 0, 0, 0x2000, len(threaded), 0x8000, len(weak), 0x9000, len(lazy), 0, 0
        )
        slice_data = self.parse(
            macho(cpu=12, subtype=9, extras=[cmd], blobs={0x2000: threaded, 0x8000: weak, 0x9000: lazy})
        )["slices"][0]
        self.assertTrue(slice_data["bindDecodingComplete"])
        streams = {s["kind"]: s for s in slice_data["fixupStreams"]}
        self.assertEqual(streams["bind"]["status"], "decoded")
        self.assertEqual(streams["bind"]["threadedOrdinals"], 2)
        self.assertEqual(streams["weakBind"]["decodedBinds"], 1)
        self.assertEqual(streams["lazyBind"]["decodedBinds"], 2)

    def test_section_outside_segment_rejected(self):
        data = bytearray(macho())
        struct.pack_into("<Q", data, 32 + 72 + 32, 1)
        with self.assertRaises(InputError):
            self.parse(data)

    def test_long_terminated_symbol_decodes_unterminated_rejected(self):
        # No artificial length caps: a terminated import name of any length is
        # reported verbatim; only a string with no NUL inside the slice range
        # (a genuine malformed-image condition) fails closed.
        s = self.parse(macho(imports=["_long" + "x" * 8000]))["slices"][0]
        self.assertIn("_long" + "x" * 8000, [i["name"] for i in s["imports"]])
        with self.assertRaises(InputError):
            corrupted = bytearray(macho(imports=["x" * 64]))
            # remove the terminating NUL of the import string in the strtab
            idx = corrupted.find(b"x" * 64)
            corrupted[idx + 64] = ord("y")
            self.parse(bytes(corrupted))

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
