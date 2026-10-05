"""Bounded complete-game conversion backend tests.

The hello-test and simple IPAs are committed inputs inside the bounded
convertible subset; everything else must stay honestly blocked. The Android
toolchain is simulated where SDK tools would run, so the Python wiring (source generation,
metadata, ELF/JNI packaging, zip append, alignment/signing calls and strict
self-validation) is exercised even on machines without an SDK.
"""

import importlib.util
import json
import shutil
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from radek import apk as apk_module
from radek import gamepack as gamepack_module
from radek.apk import _validate_complete_game_metadata
from radek.elf import inspect as inspect_elf
from radek.gamepack import (
    COMPLETE_GAME_ENTRY,
    COMPLETE_GAME_JNI_SYMBOL,
    _launch_message_from_cstrings,
    assess_complete_conversion,
    complete_game_metadata,
)
from radek.pipeline import Pipeline
from tests.fixtures import ipa, macho

HELLO_PATH = Path(__file__).resolve().parent.parent / "tests" / "data" / "hello-test.ipa"
SIMPLE_PATH = Path(__file__).resolve().parent.parent / "tests" / "data" / "simple.ipa"
_SIMPLE_TOOL = Path(__file__).resolve().parent.parent / "tools" / "make_simple_ipa.py"
_SIMPLE_SPEC = importlib.util.spec_from_file_location("make_simple_ipa", _SIMPLE_TOOL)
_SIMPLE_MODULE = importlib.util.module_from_spec(_SIMPLE_SPEC)
_SIMPLE_SPEC.loader.exec_module(_SIMPLE_MODULE)
SIMPLE_CODE = _SIMPLE_MODULE.CODE
SIMPLE_RETURN_VALUE = _SIMPLE_MODULE.RETURN_VALUE
SIMPLE_LAUNCH_MESSAGE = _SIMPLE_MODULE.LAUNCH_MESSAGE


def _fake_toolchain(root: Path):
    from radek.apk import Toolchain

    sdk = root / "sdk"
    build = sdk / "build-tools" / "35.0.0"
    build.mkdir(parents=True)
    platform = sdk / "platforms" / "android-35"
    platform.mkdir(parents=True)
    (platform / "android.jar").write_bytes(b"fake")
    for name in ("aapt2", "zipalign", "apksigner", "d8"):
        (build / name).write_text("#!/bin/sh\nexit 0\n")
        (build / name).chmod(0o755)
    return Toolchain(sdk, build)


def _fake_run_factory(calls: list):
    from tests.test_dex import dex

    def fake_run(args, log=None, timeout=180):
        argv = [str(a) for a in args]
        calls.append(argv)
        tool = Path(argv[0]).name
        if tool == "aapt2" and "dump" in argv:
            apk_path = argv[-1]
            with zipfile.ZipFile(apk_path) as package:
                metadata = json.loads(package.read("assets/conversion.json"))
            icon = next(n for n in package.namelist() if n.startswith("res/") and n.endswith(".png"))
            return (
                f"package: name='{metadata['package']}' versionCode='1' versionName='1.0'\n"
                f"launchable-activity: name='{COMPLETE_GAME_ENTRY}'  label='Converted'\n"
                f"application-icon-160:'{icon}'\n"
            )
        if tool == "aapt2" and "link" in argv:
            out = Path(argv[argv.index("-o") + 1])
            gen = Path(argv[argv.index("--java") + 1])
            with zipfile.ZipFile(out, "w") as package:
                package.writestr("AndroidManifest.xml", b"\x03\x00" + bytes(6))
                package.writestr("resources.arsc", b"\x00\x01\x02\x03")
                package.writestr("res/drawable-nodpi/converted_icon.png", b"\x89PNG-fake-icon")
            (gen / "dev" / "radek" / "generated").mkdir(parents=True)
            (gen / "dev" / "radek" / "generated" / "R.java").write_text(
                "package dev.radek.generated; final class R {}"
            )
            return ""
        if tool == "d8":
            out = Path(argv[argv.index("--output") + 1])
            (out / "classes.dex").write_bytes(bytes(dex()))
            return ""
        if tool == "zipalign" and "-c" not in argv:
            shutil.copyfile(argv[-2], argv[-1])
            return ""
        if tool == "apksigner" and "sign" in argv:
            shutil.copyfile(argv[-1], Path(argv[argv.index("--out") + 1]))
            return ""
        return ""

    return fake_run


class HelloIpaEligibilityTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(HELLO_PATH.is_file(), "committed hello-test IPA is missing")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_analysis_reports_full_text_coverage(self):
        report = Pipeline(self.root / "analysis").run(HELLO_PATH, True, analyze_only=True)
        self.assertEqual(report["state"], "PARTIAL")
        self.assertEqual(report["portProgress"]["percent"], 100.0)
        self.assertEqual(report["portProgress"]["recompiledTextBytes"], 12)
        self.assertFalse(report["portProgress"]["completeGameConversion"])

    def test_hello_ipa_is_eligible_and_carries_launch_message(self):
        output = self.root / "analysis"
        report = Pipeline(output).run(HELLO_PATH, True, analyze_only=True)
        executable = output / "extracted"
        self.assertFalse(executable.exists())  # temporary workspace removed
        # Rebuild the same inputs the converter uses from the committed IPA.
        import zipfile as zipfile_module

        with zipfile_module.ZipFile(HELLO_PATH) as archive:
            data = archive.read("Payload/Fixture.app/Fixture")
        from radek.analysis import analyze as analyze_binary, dependency_graph, prove_leaf
        from radek.recon import reconstruct

        tmp_app = self.root / "app"
        (tmp_app).mkdir()
        (tmp_app / "Fixture").write_bytes(data)
        (tmp_app / "Info.plist").write_bytes(b"unused")
        mach = analyze_binary(tmp_app / "Fixture")
        graph = dependency_graph(tmp_app, tmp_app / "Fixture", mach)
        reconstruction = reconstruct(tmp_app, {n["path"]: n["analysis"] for n in graph["nodes"]})
        selected, program = prove_leaf(tmp_app / "Fixture", mach, graph, reconstruction, "auto")
        assessment = assess_complete_conversion(
            tmp_app, "Fixture", data, mach, graph, reconstruction, selected, program, "UNAVAILABLE"
        )
        self.assertTrue(assessment["eligible"], assessment["reasons"])
        self.assertEqual(assessment["launchMessage"], "hello test succesfull")
        paths = {item["path"] for item in assessment["resourceInventory"]}
        self.assertIn("Info.plist", paths)
        self.assertNotIn("Fixture", paths)

    def test_icon_bearing_ipa_stays_blocked_on_host(self):
        source = ipa(self.root / "with-icon.ipa")
        output = self.root / "analysis-icon"
        report = Pipeline(output).run(source, True, analyze_only=True)
        self.assertEqual(report["icon"]["status"], "SUPPORTED")
        import zipfile as zipfile_module

        with zipfile_module.ZipFile(source) as archive:
            data = archive.read("Payload/Fixture.app/Fixture")
        from radek.analysis import analyze as analyze_binary, dependency_graph, prove_leaf
        from radek.recon import reconstruct

        tmp_app = self.root / "app-icon"
        tmp_app.mkdir()
        (tmp_app / "Fixture").write_bytes(data)
        mach = analyze_binary(tmp_app / "Fixture")
        graph = dependency_graph(tmp_app, tmp_app / "Fixture", mach)
        reconstruction = reconstruct(tmp_app, {n["path"]: n["analysis"] for n in graph["nodes"]})
        selected, program = prove_leaf(tmp_app / "Fixture", mach, graph, reconstruction, "auto")
        assessment = assess_complete_conversion(
            tmp_app, "Fixture", data, mach, graph, reconstruction, selected, program, "SUPPORTED"
        )
        self.assertFalse(assessment["eligible"])
        self.assertTrue(any("icon" in reason for reason in assessment["reasons"]))

    def test_import_bearing_sample_stays_ineligible(self):
        import struct

        code = struct.pack("<II", 0x52800540, 0xD65F03C0)
        source = ipa(self.root / "imports.ipa", macho(code, imports=("_UIView",)), icon=False)
        with zipfile.ZipFile(source) as archive:
            data = archive.read("Payload/Fixture.app/Fixture")
        from radek.analysis import analyze as analyze_binary, dependency_graph, prove_leaf
        from radek.recon import reconstruct

        tmp_app = self.root / "app-imports"
        tmp_app.mkdir()
        (tmp_app / "Fixture").write_bytes(data)
        mach = analyze_binary(tmp_app / "Fixture")
        graph = dependency_graph(tmp_app, tmp_app / "Fixture", mach)
        reconstruction = reconstruct(tmp_app, {n["path"]: n["analysis"] for n in graph["nodes"]})
        selected, program = prove_leaf(tmp_app / "Fixture", mach, graph, reconstruction, "auto")
        assessment = assess_complete_conversion(
            tmp_app, "Fixture", data, mach, graph, reconstruction, selected, program, "UNAVAILABLE"
        )
        self.assertFalse(assessment["eligible"])
        self.assertTrue(any("import" in reason for reason in assessment["reasons"]))


class SimpleIpaFixtureTests(unittest.TestCase):
    def test_committed_fixture_regenerates_deterministically_and_is_substantially_larger(self):
        self.assertTrue(SIMPLE_PATH.is_file(), "committed simple.ipa fixture is missing")
        self.assertEqual(len(SIMPLE_CODE), 1072)
        self.assertGreater(len(SIMPLE_CODE), 80 * 12)  # hello-test has a 12-byte entry.
        self.assertEqual(SIMPLE_LAUNCH_MESSAGE, "Simple IPA: 128 integer operations statically recompiled")
        with tempfile.TemporaryDirectory() as directory:
            regenerated = ipa(
                Path(directory) / "simple.ipa",
                macho(SIMPLE_CODE, cstring=SIMPLE_LAUNCH_MESSAGE.encode("ascii") + bytes([0])),
                icon=False,
                display_name="Simple IPA",
                bundle_id="dev.radek.simpleipa",
            )
            self.assertEqual(SIMPLE_PATH.read_bytes(), regenerated.read_bytes())


class CompleteGameMetadataTests(unittest.TestCase):
    def test_metadata_satisfies_strict_validator(self):
        metadata = complete_game_metadata(
            {"name": "Hello Test", "originalName": "hello-test.ipa", "bundleId": "dev.radek.hellotest"},
            "ab" * 32,
            "arm64-v8a",
            b"\x1f\x03\x80\xd6",
            [{"path": "Info.plist", "sha256": "0" * 64}],
            "hello test succesfull",
        )
        game = _validate_complete_game_metadata(
            metadata, "dev.radek.converted.p" + "ab" * 10, "arm64-v8a"
        )
        self.assertEqual(game["status"], "COMPLETE")
        self.assertEqual(metadata["launchMessage"], "hello test succesfull")
        self.assertEqual(metadata["conversion"]["outputBytes"], 4)

    def test_jni_symbol_is_a_valid_elf_export(self):
        from radek.elf_writer import build_shared_object

        code = bytes.fromhex("4002805221040011c0035fd6")
        blob = build_shared_object(code, "arm64", symbol=COMPLETE_GAME_JNI_SYMBOL)
        report = inspect_elf(blob)
        self.assertEqual(report["architecture"], "arm64-v8a")
        export = report["exports"][COMPLETE_GAME_JNI_SYMBOL]
        self.assertEqual(export["type"], 2)  # STT_FUNC
        self.assertEqual(export["size"], len(code))
        import hashlib

        self.assertEqual(export["sha256"], hashlib.sha256(code).hexdigest())
        self.assertEqual(report["undefinedSymbols"], [])


class CompleteGameBuildSimulationTests(unittest.TestCase):
    """Full convert path with simulated SDK tools: READY state and real checks."""

    def setUp(self):
        self.assertTrue(HELLO_PATH.is_file(), "committed hello-test IPA is missing")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_simulated_toolchain_converts_hello_ipa_to_ready(self):
        calls: list = []
        tools = _fake_toolchain(self.root)
        fake_run = _fake_run_factory(calls)
        with patch.object(apk_module, "run", side_effect=fake_run), patch.object(
            gamepack_module, "run", side_effect=fake_run
        ), patch.object(apk_module.Toolchain, "discover", return_value=tools):
            report = Pipeline(self.root / "conversion").run(HELLO_PATH, True, analyze_only=False)

        self.assertEqual(report["state"], "READY", report.get("error"))
        self.assertEqual(report["conversionProgress"]["percent"], 100)
        self.assertEqual(report["conversionProgress"]["status"], "READY")
        self.assertTrue(report["portProgress"]["completeGameConversion"])
        complete = report["completeConversion"]
        self.assertEqual(complete["status"], "COMPLETE")
        self.assertEqual(complete["contract"], "complete-game-v1")
        self.assertEqual(complete["launchMessage"], "hello test succesfull")
        self.assertEqual(complete["artifact"], "hello-test.apk")
        self.assertEqual(complete["notRecompiledReachableFunctions"], 0)
        apk = self.root / "conversion" / "hello-test.apk"
        self.assertTrue(apk.is_file())
        with zipfile.ZipFile(apk) as package:
            names = package.namelist()
            self.assertIn("lib/arm64-v8a/libconverted.so", names)
            self.assertIn("assets/conversion.json", names)
            self.assertIn("assets/bundle/Info.plist", names)
            metadata = json.loads(package.read("assets/conversion.json"))
            library = inspect_elf(package.read("lib/arm64-v8a/libconverted.so"))
        self.assertEqual(metadata["contract"], "complete-game-v1")
        self.assertEqual(metadata["source"]["sha256"], report["application"]["sha256"])
        export = library["exports"][COMPLETE_GAME_JNI_SYMBOL]
        self.assertEqual(export["size"], metadata["conversion"]["outputBytes"])
        self.assertEqual(export["sha256"], metadata["conversion"]["machineCodeSha256"])
        tool_names = [Path(c[0]).name for c in calls]
        for expected in ("aapt2", "javac", "d8", "zipalign", "apksigner", "keytool"):
            self.assertIn(expected, tool_names)

    def test_simulated_toolchain_converts_simple_ipa_to_ready(self):
        self.assertTrue(SIMPLE_PATH.is_file(), "committed simple.ipa fixture is missing")
        calls: list = []
        tools = _fake_toolchain(self.root)
        fake_run = _fake_run_factory(calls)
        with patch.object(apk_module, "run", side_effect=fake_run), patch.object(
            gamepack_module, "run", side_effect=fake_run
        ), patch.object(apk_module.Toolchain, "discover", return_value=tools):
            report = Pipeline(self.root / "simple-conversion").run(SIMPLE_PATH, True, analyze_only=False)

        self.assertEqual(report["state"], "READY", report.get("error"))
        self.assertEqual(report["conversionProgress"]["percent"], 100)
        complete = report["completeConversion"]
        self.assertEqual(complete["status"], "COMPLETE")
        self.assertEqual(complete["artifact"], "simple.apk")
        self.assertEqual(complete["launchMessage"], SIMPLE_LAUNCH_MESSAGE)
        apk = self.root / "simple-conversion" / "simple.apk"
        self.assertTrue(apk.is_file())
        with zipfile.ZipFile(apk) as package:
            metadata = json.loads(package.read("assets/conversion.json"))
            library = inspect_elf(package.read("lib/arm64-v8a/libconverted.so"))
        export = library["exports"][COMPLETE_GAME_JNI_SYMBOL]
        self.assertEqual(metadata["conversion"]["outputBytes"], len(SIMPLE_CODE))
        self.assertEqual(export["size"], len(SIMPLE_CODE))
        self.assertEqual(export["sha256"], metadata["conversion"]["machineCodeSha256"])
        self.assertEqual(metadata["launchMessage"], SIMPLE_LAUNCH_MESSAGE)

    def test_convert_without_toolchain_stays_blocked_honestly(self):
        def no_toolchain():
            raise RuntimeError("no sdk")

        with patch.object(apk_module.Toolchain, "discover", side_effect=no_toolchain):
            report = Pipeline(self.root / "conversion").run(HELLO_PATH, True, analyze_only=False)
        self.assertEqual(report["state"], "BLOCKED")
        self.assertEqual(report["conversionProgress"]["status"], "NOT_BUILT")
        self.assertEqual(
            report["completeConversion"]["status"], "ELIGIBLE_NO_ANDROID_TOOLCHAIN"
        )
        self.assertFalse(report["completeConversion"]["completeGameConversion"])
        self.assertFalse(list((self.root / "conversion").glob("*.apk")))

    def test_sample_leaf_stays_blocked_even_with_toolchain(self):
        sample = Path(__file__).resolve().parent.parent / "tests" / "data" / "sample-leaf.ipa"
        calls: list = []
        tools = _fake_toolchain(self.root)
        fake_run = _fake_run_factory(calls)
        with patch.object(apk_module, "run", side_effect=fake_run), patch.object(
            gamepack_module, "run", side_effect=fake_run
        ), patch.object(apk_module.Toolchain, "discover", return_value=tools):
            report = Pipeline(self.root / "conversion").run(sample, True, analyze_only=False)
        self.assertEqual(report["state"], "BLOCKED")
        self.assertEqual(report["conversionProgress"]["status"], "NOT_BUILT")
        self.assertEqual(report["completeConversion"]["status"], "INELIGIBLE")
        self.assertFalse(report["completeConversion"]["completeGameConversion"])


class NativeTrivialProverTests(unittest.TestCase):
    """Run the real native prover (the same code the Android app calls)."""

    def setUp(self):
        self.assertTrue(HELLO_PATH.is_file(), "committed hello-test IPA is missing")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def _run(self, payload: bytes) -> dict:
        import subprocess

        from radek.analysis import analyzer_path

        binary = Path(self.tmp.name) / "executable"
        binary.write_bytes(payload)
        proc = subprocess.run(
            [str(analyzer_path()), str(binary), "trivial"],
            text=True,
            capture_output=True,
            timeout=60,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return json.loads(proc.stdout)

    def test_hello_executable_is_proven_end_to_end(self):
        import base64
        import hashlib

        with zipfile.ZipFile(HELLO_PATH) as archive:
            payload = archive.read("Payload/Fixture.app/Fixture")
        result = self._run(payload)
        self.assertEqual(result["status"], "PROVEN", result.get("reason"))
        self.assertEqual(result["targetAbi"], "arm64-v8a")
        self.assertEqual(result["sourceBytes"], 12)
        self.assertEqual(result["textBytes"], 12)
        self.assertEqual(result["coveragePercent"], 100)
        self.assertEqual(result["functionCount"], 1)
        self.assertIn("hello test succesfull", result["strings"])
        machine_code = base64.b64decode(result["machineCode"])
        self.assertEqual(machine_code, payload[0x1000 : 0x1000 + 12])
        self.assertEqual(len(machine_code), result["sourceBytes"])

    def test_simple_ipa_has_a_long_proven_entry_and_matching_host_semantics(self):
        import base64
        import ctypes
        import hashlib
        import os
        import subprocess
        from radek.c_backend import emit
        from radek.ir import lift

        self.assertTrue(SIMPLE_PATH.is_file(), "committed simple.ipa fixture is missing")
        with zipfile.ZipFile(SIMPLE_PATH) as archive:
            payload = archive.read("Payload/Fixture.app/Fixture")
        result = self._run(payload)
        self.assertEqual(result["status"], "PROVEN", result.get("reason"))
        self.assertEqual(result["sourceBytes"], len(SIMPLE_CODE))
        self.assertEqual(result["textBytes"], len(SIMPLE_CODE))
        self.assertEqual(result["coveragePercent"], 100)
        self.assertEqual(result["functionCount"], 1)
        self.assertIn(SIMPLE_LAUNCH_MESSAGE, result["strings"])
        machine_code = base64.b64decode(result["machineCode"])
        self.assertEqual(machine_code, payload[0x1000:0x1000 + len(SIMPLE_CODE)])
        self.assertEqual(hashlib.sha256(machine_code).hexdigest(), lift(SIMPLE_CODE, "arm64").report()["machineCodeSha256"])

        if not shutil.which(os.environ.get("CXX", "g++")):
            self.skipTest("C++ compiler unavailable for host semantic execution")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "simple.c"
            library = Path(directory) / "simple.so"
            source.write_text(emit(lift(SIMPLE_CODE, "arm64")), encoding="utf-8")
            subprocess.run(
                [os.environ.get("CXX", "g++"), "-std=c++17", "-x", "c++", "-shared", "-fPIC",
                 "-Wall", "-Wextra", "-Werror", str(source), "-o", str(library)],
                check=True,
                capture_output=True,
                text=True,
            )
            native_entry = ctypes.CDLL(str(library)).radek_recompiled_entry
            native_entry.restype = ctypes.c_uint32
            self.assertEqual(native_entry(), SIMPLE_RETURN_VALUE)

    def test_sample_leaf_executable_is_rejected_for_full_coverage(self):
        import struct

        sample = Path(__file__).resolve().parent.parent / "tests" / "data" / "sample-leaf.ipa"
        with zipfile.ZipFile(sample) as archive:
            payload = archive.read("Payload/Fixture.app/Fixture")
        result = self._run(payload)
        self.assertEqual(result["status"], "UNSUPPORTED")
        self.assertTrue(result["reason"])

    def test_garbage_is_rejected(self):
        result = self._run(b"\x00\x01\x02\x03" * 16)
        self.assertEqual(result["status"], "UNSUPPORTED")


class LaunchMessageTests(unittest.TestCase):
    def test_longest_printable_cstring_wins(self):
        selected = {
            "segments": [
                {
                    "sections": [
                        {"name": "__cstring", "offset": 0, "size": 30},
                    ]
                }
            ]
        }
        data = b"ab\x00hello test succesfull\x00\x01\x02bad\x00"
        self.assertEqual(_launch_message_from_cstrings(data, selected), "hello test succesfull")

    def test_missing_section_is_empty(self):
        self.assertEqual(_launch_message_from_cstrings(b"", {"segments": []}), "")


if __name__ == "__main__":
    unittest.main()
