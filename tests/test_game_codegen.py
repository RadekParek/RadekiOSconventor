"""Coverage accounting and Android-link evidence for the whole-game lifter."""

import io
import json
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace

from radek.game.android_linker import (
    link_generated_translation,
    prepare_translated_game_runtime_input,
    verify_android_translation,
)
from radek.game.codegen import _translated_text_coverage
from radek.elf_writer import build_shared_object

ENTRY_POINT = "Java_dev_radek_gameruntime_GameBootActivity_runTranslatedGame"


class TranslatedTextCoverageTests(unittest.TestCase):
    def test_counts_only_successfully_emitted_unique_bytes_inside_text(self):
        functions = {
            0x1000: SimpleNamespace(
                instructions=[
                    (0x1000, "mov", "r0, r0", b"abcd"),
                    (0x1004, "bx", "lr", b"efgh"),
                ]
            ),
            0x1002: SimpleNamespace(
                instructions=[(0x1002, "mov", "r1, r1", b"wxyz")]
            ),
            # A failed or skipped function is absent from translated_addrs and
            # must not inflate the successful-emission numerator.
            0x1008: SimpleNamespace(
                instructions=[(0x1000, "ldr", "r0, [sp]", b"FAIL")]
            ),
        }

        report = _translated_text_coverage(functions, [0x1000, 0x1002], 0x1000, 8)

        self.assertEqual(report["uniqueTextBytes"], 8)
        self.assertEqual(report["summedFunctionInstructionBytes"], 12)
        self.assertEqual(report["summedTextInstructionBytes"], 12)
        self.assertEqual(report["overlappingTextInstructionBytes"], 4)
        self.assertEqual(report["translatedInstructionCount"], 3)

    def test_clips_instruction_ranges_to_executable_text(self):
        functions = {
            1: SimpleNamespace(instructions=[(0x0FFE, "ldr", "r0, [pc]", b"abcdefgh")]),
        }
        report = _translated_text_coverage(functions, [1], 0x1000, 4)
        self.assertEqual(report["summedFunctionInstructionBytes"], 8)
        self.assertEqual(report["summedTextInstructionBytes"], 4)
        self.assertEqual(report["uniqueTextBytes"], 4)


class AndroidLinkVerificationTests(unittest.TestCase):
    def test_android_elf_must_export_every_translated_function(self):
        elf = build_shared_object(b"\x1f\x20\x03\xd5", "arm64", symbol="t_main_1000")
        source_report = {
            "functions": 1,
            "functionFailures": 0,
            "translatedFunctionSymbols": ["t_main_1000"],
            "translationEntryPointSymbol": "t_main_1000",
            "translatedUniqueTextBytes": 4,
            "executableTextBytes": 16,
        }

        verified = verify_android_translation(elf, source_report, "arm64-v8a")

        self.assertEqual(verified["status"], "VERIFIED_ANDROID_SHARED_LIBRARY")
        self.assertEqual(verified["linkedTranslatedFunctionCount"], 1)
        self.assertEqual(verified["androidLinkedTextBytes"], 4)
        self.assertEqual(verified["androidLinkedTextPercent"], 25.0)
        self.assertTrue(verified["architectureVerified"])
        self.assertTrue(verified["dependenciesVerified"])
        self.assertTrue(verified["exportsVerified"])
        self.assertTrue(verified["allVerificationsPassed"])
        self.assertEqual(verified["dynamicDependencies"], [])
        self.assertFalse(verified["linkedIntoGame"])
        self.assertFalse(verified["apkProduced"])
        self.assertEqual(verified["apiRelinking"]["staticGuestImportRewriteCount"], 0)

    def test_android_elf_must_export_a_sized_game_boot_jni_entry_point(self):
        elf = build_shared_object(b"\x1f\x20\x03\xd5", "arm64", symbol="t_main_1000")
        source_report = {
            "functions": 1,
            "functionFailures": 0,
            "translatedFunctionSymbols": ["t_main_1000"],
            "translationEntryPointSymbol": "Java_dev_radek_gameruntime_GameBootActivity_runTranslatedGame",
            "translatedUniqueTextBytes": 4,
            "executableTextBytes": 16,
        }

        blocked = verify_android_translation(elf, source_report, "arm64-v8a")

        self.assertEqual(blocked["status"], "BLOCKED_TRANSLATION_ENTRYPOINT_VERIFICATION")
        self.assertFalse(blocked["translationEntryPointVerified"])
        self.assertEqual(blocked["androidLinkedTextBytes"], 0)
        self.assertFalse(blocked["allVerificationsPassed"])

    def test_android_elf_with_a_missing_function_symbol_counts_zero(self):
        elf = build_shared_object(b"\x1f\x20\x03\xd5", "arm64", symbol="some_other_function")
        source_report = {
            "functions": 1,
            "functionFailures": 0,
            "translatedFunctionSymbols": ["t_missing_1000"],
            "translatedUniqueTextBytes": 4,
            "executableTextBytes": 16,
        }

        blocked = verify_android_translation(elf, source_report, "arm64-v8a")

        self.assertEqual(blocked["status"], "BLOCKED_TRANSLATED_SYMBOL_VERIFICATION")
        self.assertEqual(blocked["androidLinkedTextBytes"], 0)
        self.assertEqual(blocked["androidLinkedTextPercent"], 0)
        self.assertEqual(blocked["missingFunctionSymbols"], ["t_missing_1000"])

    def test_unreviewed_android_elf_dependency_counts_zero(self):
        elf = bytearray(build_shared_object(b"\x1f\x20\x03\xd5", "arm64", symbol="t_main_1000"))
        phoff = struct.unpack_from("<Q", elf, 32)[0]
        phentsize, phnum = struct.unpack_from("<HH", elf, 54)
        dynamic_offset = None
        for index in range(phnum):
            header = phoff + index * phentsize
            kind, _flags, file_offset, _vaddr, _paddr, _filesz, _memsz, _align = struct.unpack_from(
                "<IIQQQQQQ", elf, header
            )
            if kind == 2:
                dynamic_offset = file_offset
                break
        self.assertIsNotNone(dynamic_offset)
        # Reclassify DT_HASH as DT_NEEDED at string-table offset 1. The inspector
        # then sees a dependency name not in the reviewed Android allowlist.
        for offset in range(dynamic_offset, len(elf), 16):
            tag, _value = struct.unpack_from("<qQ", elf, offset)
            if tag == 4:
                struct.pack_into("<qQ", elf, offset, 1, 1)
                break
        else:
            self.fail("ELF fixture has no DT_HASH entry to replace")

        report = verify_android_translation(
            bytes(elf),
            {
                "functions": 1,
                "functionFailures": 0,
                "translatedFunctionSymbols": ["t_main_1000"],
                "translationEntryPointSymbol": "t_main_1000",
                "translatedUniqueTextBytes": 4,
                "executableTextBytes": 16,
            },
            "arm64-v8a",
        )

        self.assertEqual(report["status"], "BLOCKED_ANDROID_ELF_VALIDATION")
        self.assertEqual(report["androidLinkedTextBytes"], 0)
        self.assertFalse(report["dependenciesVerified"])

    def test_wrong_elf_architecture_or_class_counts_zero(self):
        elf = build_shared_object(b"\x1f\x20\x03\xd5", "arm64", symbol="t_main_1000")
        report = verify_android_translation(
            elf,
            {
                "functions": 1,
                "functionFailures": 0,
                "translatedFunctionSymbols": ["t_main_1000"],
                "translationEntryPointSymbol": "t_main_1000",
                "translatedUniqueTextBytes": 4,
                "executableTextBytes": 16,
            },
            "armeabi-v7a",
        )

        self.assertEqual(report["status"], "BLOCKED_ANDROID_ELF_VALIDATION")
        self.assertEqual(report["androidLinkedTextBytes"], 0)
        self.assertFalse(report["architectureVerified"])
        self.assertFalse(report["allVerificationsPassed"])

    def test_inconsistent_translation_count_counts_zero(self):
        elf = build_shared_object(b"\x1f\x20\x03\xd5", "arm64", symbol="t_main_1000")
        report = verify_android_translation(
            elf,
            {
                "functions": 2,
                "functionFailures": 0,
                "translatedFunctionSymbols": ["t_main_1000"],
                "translationEntryPointSymbol": "t_main_1000",
                "translatedUniqueTextBytes": 4,
                "executableTextBytes": 16,
            },
            "arm64-v8a",
        )

        self.assertEqual(report["status"], "BLOCKED_TRANSLATED_SYMBOL_MANIFEST")
        self.assertEqual(report["androidLinkedTextPercent"], 0)

    def test_no_ndk_is_a_zero_progress_link_result(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("game_all.c", "rt_gen.c", "rt_gen.h"):
                (root / name).write_text("/* test input */\n", encoding="utf-8")
            (root / "rt_report.json").write_text(
                json.dumps(
                    {
                        "functions": 1,
                        "functionFailures": 0,
                        "translatedFunctionSymbols": ["t_main_1000"],
                        "translationEntryPointSymbol": "t_main_1000",
                        "translatedUniqueTextBytes": 4,
                        "executableTextBytes": 16,
                    }
                ),
                encoding="utf-8",
            )

            report = link_generated_translation(
                root,
                "arm64-v8a",
                environ={"ANDROID_NDK_HOME": str(root / "missing-ndk")},
            )

        self.assertEqual(report["status"], "BLOCKED_NO_ANDROID_NDK")
        self.assertFalse(report["ndkLinkVerified"])
        self.assertEqual(report["linkedTranslatedFunctionCount"], 0)
        self.assertEqual(report["androidLinkedTextBytes"], 0)
        self.assertEqual(report["androidLinkedTextPercent"], 0)
        self.assertFalse(report["apkProduced"])

    def test_verified_translation_can_be_prepared_for_game_runtime_apk_import(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report_path = root / "rt_report.json"
            memory_path = root / "rt_mem.bin"
            artifact_path = root / "android-link" / "arm64-v8a" / "libtranslated_game.so"
            artifact_path.parent.mkdir(parents=True)
            elf = build_shared_object(bytes.fromhex("1f2003d5"), "arm64", symbol=ENTRY_POINT)
            source_report = {
                "functions": 1,
                "functionFailures": 0,
                "translatedFunctionSymbols": [ENTRY_POINT],
                "translationEntryPointSymbol": ENTRY_POINT,
                "translatedUniqueTextBytes": 4,
                "executableTextBytes": 16,
                "sourceExecutableSha256": "a" * 64,
            }
            report_path.write_text(json.dumps(source_report), encoding="utf-8")
            memory_path.write_bytes(b"guest-memory")
            artifact_path.write_bytes(elf)
            link_report = verify_android_translation(elf, source_report, "arm64-v8a")
            link_report.update({
                "attempted": True,
                "ndkLinkVerified": True,
                "artifact": "android-link/arm64-v8a/libtranslated_game.so",
            })

            prepared = prepare_translated_game_runtime_input(root, "arm64-v8a", link_report)

            self.assertEqual(prepared["status"], "READY_FOR_GAME_RUNTIME_APK")
            self.assertFalse(prepared["linkedIntoGame"])
            self.assertFalse(prepared["apkProduced"])
            with zipfile.ZipFile(root / prepared["runtimeInputBundle"]) as bundle:
                self.assertEqual(set(bundle.namelist()), {
                    "manifest.json", "libtranslated_game.so", "translated-game-payload.zip",
                    "android-link-report.json", "rt_report.json",
                })
                bundle_manifest = json.loads(bundle.read("manifest.json"))
                self.assertEqual(bundle_manifest["contract"], "translated-game-runtime-input-v1")
                payload_bytes = bundle.read("translated-game-payload.zip")
            with zipfile.ZipFile(io.BytesIO(payload_bytes)) as payload:
                payload_manifest = json.loads(payload.read("manifest.json"))
                self.assertEqual(payload_manifest["contract"], "translated-game-payload-v1")
                self.assertEqual(payload.read("rt_mem.bin"), b"guest-memory")

    def test_runtime_input_is_blocked_when_entrypoint_verification_is_missing(self):
        blocked = prepare_translated_game_runtime_input(
            ".", "arm64-v8a", {"status": "VERIFIED_ANDROID_SHARED_LIBRARY"}
        )
        self.assertEqual(blocked["status"], "BLOCKED_TRANSLATED_GAME_PACKAGE_INPUT")
        self.assertFalse(blocked["readyForGameRuntimeApk"])
        self.assertFalse(blocked["linkedIntoGame"])


class CapstoneArmDetailTests(unittest.TestCase):
    def test_stack_ldr_has_memory_detail_operand(self):
        try:
            import capstone
            from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs
            from capstone.arm import ARM_OP_MEM
        except ImportError:
            self.skipTest("capstone is not installed")

        from radek.game.disasm import require_capstone

        version = tuple(map(int, capstone.__version__.split(".")[:3]))
        if version < (5, 0, 6) or version >= (6, 0, 0):
            with self.assertRaisesRegex(RuntimeError, "capstone >= 5.0.6,<6.0.0"):
                require_capstone()
            return
        require_capstone()
        engine = Cs(CS_ARCH_ARM, CS_MODE_ARM)
        engine.detail = True
        instructions = list(engine.disasm(bytes.fromhex("00309de5"), 0x2C7C))
        self.assertEqual(len(instructions), 1)
        self.assertEqual(instructions[0].mnemonic, "ldr")
        self.assertTrue(any(operand.type == ARM_OP_MEM for operand in instructions[0].operands[1:]))
        version = tuple(map(int, capstone.__version__.split(".")[:3]))
        self.assertGreaterEqual(version, (5, 0, 6))
        self.assertLess(version, (6, 0, 0))


if __name__ == "__main__":
    unittest.main()
