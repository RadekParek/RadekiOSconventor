import os
import shutil
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path
from radek.apk import Toolchain, elf_info, validate_apk, ARTIFACT
from radek.archive import InputError
from radek.pipeline import Pipeline
from .fixtures import ipa, macho


class ELFTests(unittest.TestCase):
    def test_not_elf_rejected(self):
        with self.assertRaises(InputError):
            elf_info(b"bad")

    def test_wrong_machine_rejected(self):
        b = bytearray(64)
        b[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<HH", b, 16, 3, 62)
        with self.assertRaises(InputError):
            elf_info(b)

    def test_missing_apk_rejected_before_tools(self):
        with self.assertRaises(InputError):
            validate_apk(Path("/not-an-apk"), None, "p", "entry")

    def test_incomplete_apk_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "bad.apk"
            with zipfile.ZipFile(p, "w") as z:
                z.writestr("AndroidManifest.xml", "not real")
            with self.assertRaises(InputError):
                validate_apk(p, None, "p", "entry")


class AndroidIntegrationTests(unittest.TestCase):
    """Not mocked. Required in CI; explicit skips only when local SDK is absent."""

    @classmethod
    def setUpClass(cls):
        try:
            cls.tools = Toolchain.discover()
        except RuntimeError as exc:
            if os.environ.get("RADEK_REQUIRE_ANDROID") == "1":
                raise
            raise unittest.SkipTest(str(exc))
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.key = cls.root / "debug.keystore"

    def convert(self, name, binary):
        source = ipa(self.root / f"{name}.ipa", binary)
        result = Pipeline(self.root / name).run(source, True, key=self.key)
        self.assertEqual(result["state"], "READY", result.get("error") or result.get("blockers"))
        self.assertEqual(result["validation"]["status"], "PASSED")
        output = self.root / name / ARTIFACT
        self.assertTrue(output.is_file())
        with zipfile.ZipFile(output) as z:
            native = z.read("lib/arm64-v8a/libconverted.so")
            self.assertEqual(elf_info(native)["needed"], [])
            self.assertNotIn("assets/bundle/Fixture", z.namelist())
            self.assertIn("assets/bundle/config.json", z.namelist())
            self.assertIn("assets/conversion.json", z.namelist())
        states = [
            e["stage"]
            for e in result["events"]
            if e["stage"] in ("IMPORTED", "ANALYZING", "CONVERTING", "PACKAGING", "VALIDATING", "READY")
        ]
        self.assertEqual(states[-1], "READY")
        self.assertIn("VALIDATING", states)
        return output, result

    def test_arm64_real_native_signed_apk(self):
        self.convert("arm64", macho())
        # No compatibility shim is needed when the proven standalone entry never
        # calls the linked framework; the generated ELF must remain import-free.
        self.convert(
            "arm64-linked-unused-framework",
            macho(
                imports=["_UIApplicationMain"],
                dependencies=["/System/Library/Frameworks/UIKit.framework/UIKit"],
            ),
        )

    def test_arm32_thumb_thumb2_real_native_apks(self):
        cases = [
            ("armv7", macho(cpu=12, subtype=9)),
            ("armv7s", macho(cpu=12, subtype=11)),
            ("armv6", macho(cpu=12, subtype=6)),
            ("armv6-thumb", macho(struct.pack("<HH", 0x202A, 0x4770), cpu=12, subtype=6, thumb=True)),
            ("thumb", macho(struct.pack("<HH", 0x202A, 0x4770), cpu=12, subtype=9, thumb=True)),
            ("thumb2", macho(struct.pack("<HHH", 0xF240, 0x002A, 0x4770), cpu=12, subtype=9, thumb=True)),
        ]
        for name, binary in cases:
            with self.subTest(name=name):
                self.convert(name, binary)

    def test_tampered_apk_signature_rejected(self):
        original, report = self.convert("tamper-source", macho())
        changed = self.root / "tampered.apk"
        with zipfile.ZipFile(original) as src, zipfile.ZipFile(changed, "w") as dst:
            for entry in src.infolist():
                # Repacking destroys v2/v3 signing block. Also edit entry DEX.
                content = src.read(entry.filename)
                if entry.filename == "classes.dex":
                    content += b"tamper"
                dst.writestr(entry, content)
        with self.assertRaises((RuntimeError, InputError)):
            validate_apk(changed, self.tools, report["output"]["package"], report["output"]["entryPoint"])
