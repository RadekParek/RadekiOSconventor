import hashlib
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile
from pathlib import Path
from radek.apk import (
    artifact_filename,
    build_apk,
    elf_info,
    validate_apk,
    _validate_complete_game_metadata,
    _validate_packaged_payloads,
)
from radek.archive import InputError


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

    def test_arm64_elf_is_parsed_and_native_symbol_is_hashed(self):
        name = b"Java_dev_radek_generated_MainActivity_runNative"
        strings = b"\0" + name + b"\0"
        text = struct.pack("<I", 0xD65F03C0)
        data = bytearray(0x400 + 4 * 64)
        text_off = 0x100
        strings_off = text_off + len(text)
        symbols_off = (strings_off + len(strings) + 7) & ~7
        dynamic_off = 0x200
        section_off = 0x400
        data[text_off : text_off + len(text)] = text
        data[strings_off : strings_off + len(strings)] = strings
        data[symbols_off : symbols_off + 24] = bytes(24)
        data[symbols_off + 24 : symbols_off + 48] = struct.pack(
            "<IBBHQQ", 1, 0x12, 0, 1, 0x1000 + text_off, len(text)
        )
        dynamic = struct.pack(
            "<qQqQqQ", 5, 0x1000 + strings_off, 10, len(strings), 0, 0
        )
        data[dynamic_off : dynamic_off + len(dynamic)] = dynamic
        ident = b"\x7fELF" + bytes((2, 1, 1, 0)) + bytes(8)
        data[:16] = ident
        struct.pack_into(
            "<HHIQQQIHHHHHH", data, 16, 3, 183, 1, 0, 64, section_off, 0,
            64, 56, 2, 64, 4, 0,
        )
        struct.pack_into(
            "<IIQQQQQQ", data, 64, 1, 5, 0, 0x1000, 0x1000, len(data), len(data), 0x1000
        )
        struct.pack_into(
            "<IIQQQQQQ", data, 120, 2, 4, dynamic_off, 0x1000 + dynamic_off,
            0x1000 + dynamic_off, len(dynamic), len(dynamic), 8,
        )
        headers = [
            (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            (0, 1, 6, 0x1000 + text_off, text_off, len(text), 0, 0, 4, 0),
            (0, 3, 2, 0x1000 + strings_off, strings_off, len(strings), 0, 0, 1, 0),
            (0, 11, 2, 0x1000 + symbols_off, symbols_off, 48, 2, 1, 8, 24),
        ]
        for index, header in enumerate(headers):
            struct.pack_into("<IIQQQQIIQQ", data, section_off + index * 64, *header)

        info = elf_info(bytes(data))
        self.assertEqual(info["architecture"], "arm64-v8a")
        self.assertEqual(info["elfClass"], 64)
        entry = info["exports"][name.decode()]
        self.assertEqual(entry["size"], len(text))
        self.assertEqual(entry["sha256"], hashlib.sha256(text).hexdigest())

    def test_arm32_elf_is_parsed_and_native_symbol_is_hashed(self):
        name = b"Java_dev_radek_generated_MainActivity_runNative"
        strings = b"\0" + name + b"\0"
        text = struct.pack("<I", 0xE12FFF1E)
        data = bytearray(0x300 + 4 * 40)
        text_off = 0x100
        strings_off = text_off + len(text)
        symbols_off = (strings_off + len(strings) + 3) & ~3
        dynamic_off = 0x200
        section_off = 0x300
        data[text_off : text_off + len(text)] = text
        data[strings_off : strings_off + len(strings)] = strings
        data[symbols_off : symbols_off + 16] = bytes(16)
        data[symbols_off + 16 : symbols_off + 32] = struct.pack(
            "<IIIBBH", 1, 0x1000 + text_off, len(text), 0x12, 0, 1
        )
        dynamic = struct.pack(
            "<iIiIiI", 5, 0x1000 + strings_off, 10, len(strings), 0, 0
        )
        data[dynamic_off : dynamic_off + len(dynamic)] = dynamic
        ident = b"\x7fELF" + bytes((1, 1, 1, 0)) + bytes(8)
        data[:16] = ident
        struct.pack_into(
            "<HHIIIIIHHHHHH", data, 16, 3, 40, 1, 0, 52, section_off, 0,
            52, 32, 2, 40, 4, 0,
        )
        struct.pack_into(
            "<IIIIIIII", data, 52, 1, 0, 0x1000, 0x1000, len(data), len(data), 5, 0x1000
        )
        struct.pack_into(
            "<IIIIIIII", data, 84, 2, dynamic_off, 0x1000 + dynamic_off, 0x1000 + dynamic_off,
            len(dynamic), len(dynamic), 4, 4,
        )
        headers = [
            (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            (0, 1, 6, 0x1000 + text_off, text_off, len(text), 0, 0, 4, 0),
            (0, 3, 2, 0x1000 + strings_off, strings_off, len(strings), 0, 0, 1, 0),
            (0, 11, 2, 0x1000 + symbols_off, symbols_off, 32, 2, 1, 4, 16),
        ]
        for index, header in enumerate(headers):
            struct.pack_into("<IIIIIIIIII", data, section_off + index * 40, *header)

        info = elf_info(bytes(data))
        self.assertEqual(info["architecture"], "armeabi-v7a")
        self.assertEqual(info["elfClass"], 32)
        entry = info["exports"][name.decode()]
        self.assertEqual(entry["size"], len(text))
        self.assertEqual(entry["sha256"], hashlib.sha256(text).hexdigest())

    def test_missing_apk_rejected_before_tools(self):
        with self.assertRaises(InputError):
            validate_apk(Path("/not-an-apk"), None, "p", "entry")

    def test_output_apk_name_uses_sanitized_ipa_basename(self):
        self.assertEqual(artifact_filename("/imports/My Game.ipa"), "My Game.apk")
        self.assertEqual(artifact_filename(r"C:\\imports\\bad:name.ipa"), "bad_name.apk")
        self.assertEqual(artifact_filename("...ipa"), "ConvertedIPA.apk")

    def test_incomplete_apk_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "bad.apk"
            with zipfile.ZipFile(p, "w") as z:
                z.writestr("AndroidManifest.xml", "not real")
            with self.assertRaises(InputError):
                validate_apk(p, None, "p", "entry")


class PackagedPayloadTests(unittest.TestCase):
    def test_source_ipa_bytes_are_rejected_even_when_renamed(self):
        source = b"PK\x03\x04synthetic source IPA bytes"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.apk"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("assets/opaque.payload", source)
            with zipfile.ZipFile(path) as archive:
                with self.assertRaisesRegex(InputError, "original IPA content"):
                    _validate_packaged_payloads(
                        archive,
                        archive.namelist(),
                        hashlib.sha256(source).hexdigest(),
                    )

    def test_apple_executable_is_rejected_outside_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.apk"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("lib/arm64-v8a/foreign.so", b"\xcf\xfa\xed\xfe payload")
            with zipfile.ZipFile(path) as archive:
                with self.assertRaisesRegex(InputError, "Apple executable"):
                    _validate_packaged_payloads(archive, archive.namelist(), "0" * 64)


class NoPlaceholderPackagingTests(unittest.TestCase):
    @staticmethod
    def _complete_game_metadata():
        source_hash = "0" * 64
        return {
            "contract": "complete-game-v1",
            "package": "dev.radek.converted.p" + source_hash[:20],
            "source": {"sha256": source_hash},
            "targetAbi": "arm64-v8a",
            "conversion": {"outputBytes": 4, "targetAbi": "arm64-v8a", "backend": "test"},
            "gameConversion": {
                "status": "COMPLETE",
                "completeGameConversion": True,
                "reachableSourceFunctions": 1,
                "recompiledReachableFunctions": 1,
                "notRecompiledReachableFunctions": 0,
                "reachableApiCount": 0,
                "generatedApiReplacements": 0,
                "nativeApiPassthroughs": 0,
                "unimplementedReachableApiCount": 0,
                "apiCoverageComplete": True,
                "apiReplacements": [],
                "resourcesComplete": True,
                "lifecycleImplemented": True,
            },
        }

    def test_restricted_leaf_wrapper_is_never_packaged_as_an_apk(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            work = root / "package-work"
            output = root / "game.apk"
            with self.assertRaisesRegex(InputError, "no complete iOS-to-Android game static recompilation backend"):
                build_apk(
                    work,
                    output,
                    b"\x00\x00\x00\x00",
                    {"sha256": "0" * 64},
                    None,
                    root / "assets",
                    {"conversion": {"outputBytes": 4}},
                    None,
                    root / "debug.keystore",
                )
            self.assertFalse(work.exists())
            self.assertFalse(output.exists())

    def test_closed_integer_entry_is_not_a_complete_game_conversion(self):
        with self.assertRaisesRegex(InputError, "not a complete-game conversion"):
            _validate_complete_game_metadata(
                {
                    "contract": "closed-integer-entry-v1",
                    "package": "dev.radek.converted.p" + "0" * 20,
                },
                "dev.radek.converted.p" + "0" * 20,
                "arm64-v8a",
            )

    def test_candidate_api_mapping_cannot_satisfy_complete_game_contract(self):
        metadata = {
            "contract": "complete-game-v1",
            "package": "dev.radek.converted.p" + "0" * 20,
            "source": {"sha256": "0" * 64},
            "targetAbi": "arm64-v8a",
            "conversion": {"outputBytes": 4, "targetAbi": "arm64-v8a", "backend": "test"},
            "gameConversion": {
                "status": "COMPLETE",
                "completeGameConversion": True,
                "reachableSourceFunctions": 1,
                "recompiledReachableFunctions": 1,
                "notRecompiledReachableFunctions": 0,
                "reachableApiCount": 1,
                "generatedApiReplacements": 0,
                "nativeApiPassthroughs": 0,
                "unimplementedReachableApiCount": 0,
                "apiCoverageComplete": True,
                "apiReplacements": [],
                "resourcesComplete": True,
                "lifecycleImplemented": True,
            },
        }
        with self.assertRaisesRegex(InputError, "API replacement accounting"):
            _validate_complete_game_metadata(
                metadata,
                "dev.radek.converted.p" + "0" * 20,
                "arm64-v8a",
            )

    def test_metadata_rejects_boolean_native_output_size(self):
        metadata = self._complete_game_metadata()
        metadata["conversion"]["outputBytes"] = True
        with self.assertRaisesRegex(InputError, "generated native-code size"):
            _validate_complete_game_metadata(metadata, metadata["package"], "arm64-v8a")

    def test_metadata_rejects_non_string_source_hash(self):
        metadata = self._complete_game_metadata()
        metadata["source"]["sha256"] = ["0" * 64]
        with self.assertRaisesRegex(InputError, "source hash"):
            _validate_complete_game_metadata(metadata, metadata["package"], "arm64-v8a")

    def test_metadata_rejects_boolean_unimplemented_api_count(self):
        metadata = self._complete_game_metadata()
        metadata["gameConversion"]["unimplementedReachableApiCount"] = False
        with self.assertRaisesRegex(InputError, "API replacement accounting"):
            _validate_complete_game_metadata(metadata, metadata["package"], "arm64-v8a")

    def test_metadata_rejects_non_string_generated_api_artifact(self):
        metadata = self._complete_game_metadata()
        game = metadata["gameConversion"]
        game.update(reachableApiCount=1, generatedApiReplacements=1)
        game["apiReplacements"] = [{
            "codeGenerated": True,
            "linkedIntoApk": True,
            "reachableFromEntry": True,
            "sourceSymbol": "_UIApplicationMain",
            "targetAndroidApi": "android.app.Activity",
            "implementationSha256": "0" * 64,
            "implementationArtifact": ["assets/generated/api.bin"],
        }]
        with self.assertRaisesRegex(InputError, "generated linked implementation evidence"):
            _validate_complete_game_metadata(metadata, metadata["package"], "arm64-v8a")

    def test_metadata_rejects_unsafe_generated_api_artifact_path(self):
        metadata = self._complete_game_metadata()
        game = metadata["gameConversion"]
        game.update(reachableApiCount=1, generatedApiReplacements=1)
        game["apiReplacements"] = [{
            "codeGenerated": True,
            "linkedIntoApk": True,
            "reachableFromEntry": True,
            "sourceSymbol": "_UIApplicationMain",
            "targetAndroidApi": "android.app.Activity",
            "implementationSha256": "0" * 64,
            "implementationArtifact": "../generated/api.bin",
        }]
        with self.assertRaisesRegex(InputError, "path is unsafe"):
            _validate_complete_game_metadata(metadata, metadata["package"], "arm64-v8a")


class ExperimentalShellTests(unittest.TestCase):
    """The experimental shell is an honestly labelled artifact package.

    It must never satisfy the complete-game contract, and its metadata and
    launcher disclosure are mandatory parts of the validation.
    """

    @staticmethod
    def _dex():
        from tests.test_dex import dex

        return bytes(dex())

    @staticmethod
    def _metadata(**overrides):
        from radek.apk import EXPERIMENTAL_SHELL_CONTRACT, EXPERIMENTAL_SHELL_NOTICE

        metadata = {
            "contract": EXPERIMENTAL_SHELL_CONTRACT,
            "generator": "RadekiOSConventor",
            "honestLabeling": True,
            "containsGameCode": False,
            "completeGameConversion": False,
            "disclosure": EXPERIMENTAL_SHELL_NOTICE,
            "provenance": {},
            "artifacts": [],
        }
        metadata.update(overrides)
        return metadata

    def _write_shell(self, path: Path, metadata=None, omit=()):
        import json

        entries = {
            "AndroidManifest.xml": b"<manifest/>",
            "classes.dex": self._dex(),
            "assets/conversion-metadata.json": json.dumps(
                metadata if metadata is not None else self._metadata()
            ).encode(),
        }
        for name in omit:
            entries.pop(name, None)
        with zipfile.ZipFile(path, "w") as package:
            for name, payload in entries.items():
                package.writestr(name, payload)

    def test_sources_carry_honest_disclosure_and_never_claim_game(self):
        from radek.apk import EXPERIMENTAL_SHELL_NOTICE, EXPERIMENTAL_SHELL_PACKAGE, experimental_shell_sources

        with tempfile.TemporaryDirectory() as directory:
            root = experimental_shell_sources(Path(directory))
            manifest = (root / "AndroidManifest.xml").read_text()
            strings = (root / "res" / "values" / "strings.xml").read_text()
            activity = (root / "src" / "ExperimentalShellActivity.java").read_text()
        self.assertIn(EXPERIMENTAL_SHELL_PACKAGE, manifest)
        self.assertIn("shell_notice", strings)
        self.assertEqual(EXPERIMENTAL_SHELL_NOTICE, "This inspection shell contains isolated analysis artifacts, not a runnable game.")
        self.assertEqual(EXPERIMENTAL_SHELL_NOTICE.count("."), 1)
        self.assertIn("shell_notice", activity)
        self.assertIn("Radek Experimental Shell", strings)
        # The launcher source is plain Java: no format-escaping leftovers.
        self.assertNotIn("{{", activity)
        self.assertIn("extends Activity {", activity)
        self.assertEqual(activity.count("{"), activity.count("}"))
        self.assertEqual(strings.count(EXPERIMENTAL_SHELL_NOTICE), 1)

    def test_valid_shell_zip_passes_sdk_free_checks(self):
        from radek.apk import validate_experimental_shell

        with tempfile.TemporaryDirectory() as directory:
            apk = Path(directory) / "experimental-shell.apk"
            self._write_shell(apk)
            result = validate_experimental_shell(apk)
        self.assertEqual(result["status"], "VALID")
        self.assertFalse(result["signatureVerified"])  # no toolchain -> not asserted

    def test_complete_game_contract_cannot_be_claimed_by_shell_metadata(self):
        from radek.apk import validate_experimental_shell

        with tempfile.TemporaryDirectory() as directory:
            apk = Path(directory) / "shell.apk"
            self._write_shell(apk, metadata=self._metadata(contract="complete-game-v1"))
            result = validate_experimental_shell(apk)
        self.assertEqual(result["status"], "INVALID")

    def test_missing_parts_are_rejected(self):
        from radek.apk import validate_experimental_shell

        cases = (
            ("classes.dex",),
            ("assets/conversion-metadata.json",),
            ("AndroidManifest.xml",),
        )
        for omitted in cases:
            with self.subTest(omitted=omitted), tempfile.TemporaryDirectory() as directory:
                apk = Path(directory) / "shell.apk"
                self._write_shell(apk, omit=omitted)
                self.assertEqual(validate_experimental_shell(apk)["status"], "INVALID")
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "nope.apk"
            self.assertEqual(validate_experimental_shell(missing)["status"], "INVALID")

    def test_stripped_disclosure_or_game_claim_is_rejected(self):
        from radek.apk import validate_experimental_shell

        with tempfile.TemporaryDirectory() as directory:
            apk = Path(directory) / "shell.apk"
            self._write_shell(apk, metadata=self._metadata(disclosure="everything works"))
            self.assertEqual(validate_experimental_shell(apk)["status"], "INVALID")
        with tempfile.TemporaryDirectory() as directory:
            apk = Path(directory) / "shell.apk"
            self._write_shell(apk, metadata=self._metadata(containsGameCode=True))
            self.assertEqual(validate_experimental_shell(apk)["status"], "INVALID")

    def test_corrupt_dex_is_rejected(self):
        from radek.apk import validate_experimental_shell

        with tempfile.TemporaryDirectory() as directory:
            apk = Path(directory) / "shell.apk"
            broken = bytearray(self._dex())
            broken[-1] ^= 1
            with zipfile.ZipFile(apk, "w") as package:
                package.writestr("AndroidManifest.xml", b"<manifest/>")
                package.writestr("classes.dex", bytes(broken))
                import json

                package.writestr(
                    "assets/conversion-metadata.json", json.dumps(self._metadata())
                )
            self.assertEqual(validate_experimental_shell(apk)["status"], "INVALID")


class ExperimentalShellBuildSimulationTests(unittest.TestCase):
    """Exercise the full build_experimental_shell glue with simulated tools.

    Each tool invocation is faked just enough to produce the artifacts the
    next stage consumes, so the Python-side wiring (aapt2 link output
    handling, javac/d8 sequencing, zip append, zipalign/apksigner calls and
    self-validation) runs exactly as in CI.
    """

    def test_simulated_toolchain_builds_and_self_validates_shell(self):
        import json
        import shutil

        from radek import apk as apk_module
        from radek.apk import Toolchain, build_experimental_shell
        from tests.test_dex import dex

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / "sdk"
            build = sdk / "build-tools" / "35.0.0"
            build.mkdir(parents=True)
            platform = sdk / "platforms" / "android-35"
            platform.mkdir(parents=True)
            (platform / "android.jar").write_bytes(b"fake")
            for name in ("aapt2", "zipalign", "apksigner", "d8"):
                (build / name).write_text("#!/bin/sh\nexit 0\n")
                (build / name).chmod(0o755)
            tools = Toolchain(sdk, build)
            calls = []

            def fake_run(args, log=None, timeout=180):
                calls.append([str(a) for a in args])
                argv = [str(a) for a in args]
                tool = Path(argv[0]).name
                if tool == "aapt2" and "link" in argv:
                    out = Path(argv[argv.index("-o") + 1])
                    gen = Path(argv[argv.index("--java") + 1])
                    with zipfile.ZipFile(out, "w") as package:
                        package.writestr("AndroidManifest.xml", b"<binary/>")
                        package.writestr("resources.arsc", b"\x00")
                    (gen / "dev" / "radek" / "experimental" / "shell").mkdir(parents=True)
                    (gen / "dev" / "radek" / "experimental" / "shell" / "R.java").write_text(
                        "package dev.radek.experimental.shell; final class R {}"
                    )
                elif tool == "d8":
                    out = Path(argv[argv.index("--output") + 1])
                    (out / "classes.dex").write_bytes(bytes(dex()))
                elif tool == "zipalign":
                    shutil.copyfile(argv[-2], argv[-1])
                elif tool == "apksigner" and "sign" in argv:
                    shutil.copyfile(argv[-1], Path(argv[argv.index("--out") + 1]))
                elif tool == "apksigner" and "verify" in argv:
                    pass
                return ""

            with patch.object(apk_module, "run", side_effect=fake_run):
                result = build_experimental_shell(
                    root / "work",
                    root / "out",
                    tools,
                    {"targetAbi": "arm64-v8a"},
                    {"librecompiled-entry.so": ("native-code", b"\x7fELF-test")},
                )
        self.assertEqual(result["status"], "BUILT_NOT_A_GAME")
        self.assertEqual(result["contract"], "experimental-shell-v1")
        self.assertTrue(result["validation"]["status"] == "VALID")
        tool_names = [Path(c[0]).name for c in calls]
        for expected in ("aapt2", "javac", "d8", "zipalign", "apksigner"):
            self.assertIn(expected, tool_names)
        self.assertIn("keytool", tool_names)
        # keytool runs before apksigner sign
        self.assertLess(tool_names.index("keytool"), len(tool_names) - 1 - tool_names[::-1].index("apksigner"))
