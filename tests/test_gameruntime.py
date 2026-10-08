"""Host tests for the game-runtime-v1 contract (radek.gameruntime)."""

import json
import os
import plistlib
import shutil
import stat
import struct
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

from radek.archive import InputError
from radek import gameruntime

REPO_ROOT = Path(__file__).resolve().parent.parent
ANGRY_BIRDS_IPA = REPO_ROOT / "tests" / "data" / "AngryBirds_v1.0_os30.ipa"


def gameboot_available() -> bool:
    binary = gameruntime.find_gameboot_binary()
    return binary is not None and binary.is_file()


class NamingTests(unittest.TestCase):
    def test_game_artifact_name_parity_with_device_contract(self):
        # Same examples as ArtifactNamesTest on the device side.
        self.assertEqual("My Game-game.apk", gameruntime.game_apk_name("../My Game.ipa"))
        self.assertEqual("bad_game-game.apk", gameruntime.game_apk_name("bad:game"))
        self.assertEqual(
            "ConvertedIPA-game.apk", gameruntime.game_apk_name("")
        )
        self.assertEqual(
            "AngryBirds_v1.0_os30-game.apk",
            gameruntime.game_apk_name("AngryBirds_v1.0_os30.ipa"),
        )

    def test_game_artifact_name_is_distinct_and_bounded(self):
        name = gameruntime.game_apk_name("Example.ipa")
        self.assertTrue(name.endswith("-game.apk"))
        self.assertNotEqual("Example.apk", name)
        self.assertNotEqual("Example-preview.apk", name)
        self.assertLessEqual(len(name), 80 + len("-game.apk"))
        long_name = gameruntime.game_apk_name("x" * 200 + ".ipa")
        self.assertLessEqual(len(long_name), 80 + len("-game.apk"))

    def test_game_package_format(self):
        package = gameruntime.game_package("a" * 64, "c" * 64)
        self.assertEqual("dev.radek.gameruntime.p" + "a" * 20 + "c" * 8, package)
        self.assertLessEqual(len(package), 127)
        with self.assertRaises(InputError):
            gameruntime.game_package("short", "c" * 64)
        with self.assertRaises(InputError):
            gameruntime.game_package("a" * 64, "not-hex!" + "c" * 56)

    def test_contract_constants_match_device_strings(self):
        self.assertEqual("game-runtime-v1", gameruntime.CONTRACT)
        self.assertEqual("dev.radek.gameruntime.GameBootActivity", gameruntime.BOOT_ACTIVITY)
        self.assertEqual(
            "Java_dev_radek_gameruntime_GameBootActivity_runGameBootAttempt",
            gameruntime.JNI_SYMBOL,
        )
        self.assertEqual("assets/gameboot/main-executable.bin", gameruntime.ASSET_EXECUTABLE)
        self.assertEqual((0xF0020000, 0xF0030000),
                         (gameruntime.TRAP_RANGE_START, gameruntime.TRAP_RANGE_END))


class MachoTests(unittest.TestCase):
    def test_thin_arm32_describes_and_stages(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            binary = tmpdir / "exec"
            binary.write_bytes(struct.pack("<IIIIIII", 0xFEEDFACE, 12, 3, 0, 0, 0, 0) + b"\x00" * 64)
            info = gameruntime.describe_macho(binary)
            self.assertEqual("thin-macho32", info["format"])
            self.assertEqual(12, info["cpuType"])
            staged = gameruntime.stage_arm_slice(binary, tmpdir / "slice.bin")
            self.assertEqual(binary.stat().st_size, staged["bytes"])
            self.assertEqual((tmpdir / "slice.bin").read_bytes(), binary.read_bytes())

    def test_thin_non_arm_is_rejected_at_stage_time(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            binary = tmpdir / "exec"
            # CPU_TYPE_ARM64 = 16777216.
            binary.write_bytes(struct.pack("<IIIIIII", 0xFEEDFACE, 16777216, 0, 0, 0, 0, 0))
            info = gameruntime.describe_macho(binary)
            self.assertEqual(16777216, info["cpuType"])
            with self.assertRaises(InputError):
                gameruntime.stage_arm_slice(binary, tmpdir / "slice.bin")

    def test_fat_selects_first_arm_slice(self):
        payload_a = b"ARM-SLICE" + b"\x00" * 23
        payload_b = b"OTHER" + b"\x00" * 27
        header = struct.pack(">II", 0xCAFEBABE, 2)
        header += struct.pack(">IIIII", 16777216, 0, 48, len(payload_b), 0)
        header += struct.pack(">IIIII", 12, 9, 48 + len(payload_b), len(payload_a), 0)
        image = header + payload_b + payload_a
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            binary = tmpdir / "fat"
            binary.write_bytes(image)
            info = gameruntime.describe_macho(binary)
            self.assertEqual("fat32", info["format"])
            self.assertEqual(2, info["sliceCount"])
            self.assertEqual(1, info["selectedSlice"])
            staged = gameruntime.stage_arm_slice(binary, tmpdir / "slice.bin")
            self.assertEqual(payload_a, (tmpdir / "slice.bin").read_bytes())
            self.assertEqual(len(payload_a), staged["bytes"])

    def test_fat_without_arm_slice_is_rejected(self):
        header = struct.pack(">II", 0xCAFEBABE, 1) + struct.pack(">IIIII", 7, 3, 28, 32, 0)
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "fat"
            binary.write_bytes(header + b"\x00" * 32)
            with self.assertRaises(InputError):
                gameruntime.describe_macho(binary)

    def test_non_macho_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "exec"
            binary.write_bytes(b"#!/bin/sh\n")
            with self.assertRaises(InputError):
                gameruntime.describe_macho(binary)
            binary.write_bytes(b"tiny")
            with self.assertRaises(InputError):
                gameruntime.describe_macho(binary)


class ProbeTests(unittest.TestCase):
    def test_missing_binary_is_not_probed(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "exec"
            target.write_bytes(b"\x00" * 32)
            probe = gameruntime.probe_boot(target, gameboot_binary=Path(tmp) / "no-such-binary")
            self.assertEqual("NOT_PROBED", probe["status"])
            self.assertIn("not built", probe["reason"])

    def test_canned_report_is_parsed(self):
        canned = {
            "status": "not_runnable",
            "trapCalls": 1,
            "trappedImport": "_UIApplicationMain",
            "reason": "stopped at the first unimplemented call",
            "loader": {"status": "LOADED_WITH_TRAPS", "resolvedSymbolCount": 39,
                       "trappedSymbolCount": 539, "unresolvedSymbolCount": 0},
            "execution": {"status": "GUEST_EXCEPTION_RAISED", "entryPointReached": True,
                          "instructions": 34},
        }
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "fake-gameboot"
            script.write_text("#!/bin/sh\necho '" + json.dumps(canned) + "'\n")
            script.chmod(script.stat().st_mode | stat.S_IXUSR)
            target = Path(tmp) / "exec"
            target.write_bytes(b"\x00" * 32)
            probe = gameruntime.probe_boot(target, gameboot_binary=script)
            self.assertEqual("PROBED", probe["status"])
            self.assertEqual("LOADED_WITH_TRAPS", probe["loaderStatus"])
            self.assertEqual(34, probe["instructions"])
            self.assertEqual("_UIApplicationMain", probe["trappedImport"])
            self.assertEqual(1, probe["trapCalls"])
            self.assertTrue(probe["entryPointReached"])

    def test_garbage_report_is_not_probed(self):
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "fake-gameboot"
            script.write_text("#!/bin/sh\necho 'not json'\n")
            script.chmod(script.stat().st_mode | stat.S_IXUSR)
            target = Path(tmp) / "exec"
            target.write_bytes(b"\x00" * 32)
            probe = gameruntime.probe_boot(target, gameboot_binary=script)
            self.assertEqual("NOT_PROBED", probe["status"])

    def test_host_probe_selects_the_finite_diagnostic_mode_explicitly(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            args_file = tmpdir / "args"
            report = {"executionPolicy": {"unlimited": False, "timeLimitMicros": 20_000_000}}
            script = tmpdir / "fake-gameboot"
            script.write_text(
                "#!/bin/sh\n"
                f"printf '%s\\n' \"$@\" > {args_file}\n"
                f"printf '%s\\n' '{json.dumps(report)}'\n"
            )
            script.chmod(script.stat().st_mode | stat.S_IXUSR)
            target = tmpdir / "exec"
            target.write_bytes(b"\x00" * 32)
            probe = gameruntime.probe_boot(target, gameboot_binary=script)
            self.assertEqual("PROBED", probe["status"])
            self.assertFalse(probe["executionPolicy"]["unlimited"])
            self.assertIn("--diagnostic-probe", args_file.read_text().splitlines())


class RuntimeExecutionPolicySourceTests(unittest.TestCase):
    def test_device_defaults_are_unlimited_and_only_host_probe_opts_in(self):
        cpu = (REPO_ROOT / "native/include/compat_runtime/cpu.hpp").read_text(encoding="utf-8")
        runner = (REPO_ROOT / "native/include/compat_runtime/runner.hpp").read_text(encoding="utf-8")
        jni = (REPO_ROOT / "native/src/compat_runtime/gameruntime_jni.cpp").read_text(encoding="utf-8")
        host = (REPO_ROOT / "native/src/compat_runtime/gameboot_main.cpp").read_text(encoding="utf-8")
        self.assertIn("instructionLimit = 0", cpu)
        self.assertIn("timeLimitMicros = 0", cpu)
        self.assertIn("entryInstructionBudget_ = 0", runner)
        self.assertIn("entryTimeLimitMicros_ = 0", runner)
        self.assertIn("if (diagnosticProbe)", host)
        self.assertIn("runner.setEntryBudget(0, 20'000'000)", host)
        self.assertNotIn("setEntryBudget", jni)


def make_synthetic_ipa(path: Path, executable_bytes: bytes) -> None:
    info = {
        "CFBundleExecutable": "Synthetic",
        "CFBundleIdentifier": "dev.radek.synthetic",
        "CFBundleName": "Synthetic",
    }
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("Payload/Synthetic.app/Info.plist", plistlib.dumps(info))
        archive.writestr("Payload/Synthetic.app/Synthetic", executable_bytes)
        archive.writestr("Payload/Synthetic.app/data.txt", b"hello")


class ManifestTests(unittest.TestCase):
    def test_manifest_shape_without_probe(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            ipa = tmpdir / "Synthetic.ipa"
            code = struct.pack("<IIIIIII", 0xFEEDFACE, 12, 9, 0, 0, 0, 0) + b"\x00" * 128
            make_synthetic_ipa(ipa, code)
            output = tmpdir / "job"
            manifest = gameruntime.run_gameboot(ipa, True, output, run_probe=False)
            on_disk = json.loads((output / "game-runtime-manifest.json").read_text())
            self.assertEqual(manifest, on_disk)
            self.assertEqual("game-runtime-v1", manifest["contract"])
            self.assertEqual("Synthetic-game.apk", manifest["artifact"]["name"])
            self.assertEqual("thin-macho32", manifest["executable"]["format"])
            self.assertEqual(len(code), manifest["executable"]["bytes"])
            self.assertEqual({"files": 2, "bytes": len(b"hello") + len(plistlib.dumps({
                "CFBundleExecutable": "Synthetic",
                "CFBundleIdentifier": "dev.radek.synthetic",
                "CFBundleName": "Synthetic"}))}, manifest["bundle"])
            self.assertEqual("NOT_PROBED", manifest["hostProbe"]["status"])
            self.assertTrue((output / "main-executable.bin").is_file())
            self.assertFalse((output / "gameboot-report.json").exists())
            summary = gameruntime.manifest_summary(manifest)
            self.assertEqual("NOT_PROBED", summary["probe"])

    def test_run_requires_authorization_and_new_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            ipa = tmpdir / "Synthetic.ipa"
            make_synthetic_ipa(ipa, b"\x00" * 64)
            with self.assertRaises(InputError):
                gameruntime.run_gameboot(ipa, False, tmpdir / "job")
            self.assertFalse((tmpdir / "job").exists())
            (tmpdir / "job").mkdir()
            with self.assertRaises(FileExistsError):
                gameruntime.run_gameboot(ipa, True, tmpdir / "job")


@unittest.skipUnless(ANGRY_BIRDS_IPA.is_file(), "Angry Birds test IPA is not checked out")
@unittest.skipUnless(gameboot_available(), "radek-gameboot host binary is not built")
class AngryBirdsBootTests(unittest.TestCase):
    def test_angry_birds_boots_to_a_documented_boundary(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "angrybirds-gameboot"
            manifest = gameruntime.run_gameboot(ANGRY_BIRDS_IPA, True, output)
            self.assertEqual("AngryBirds_v1.0_os30-game.apk", manifest["artifact"]["name"])
            self.assertEqual("thin-macho32", manifest["executable"]["format"])
            self.assertEqual(12, manifest["executable"]["cpuType"])
            self.assertEqual(
                "Payload/AngryBirds.app/AngryBirds",
                manifest["source"]["executableEntry"],
            )
            probe = manifest["hostProbe"]
            self.assertEqual("PROBED", probe["status"])
            self.assertIn(probe["loaderStatus"], {"LOADED", "LOADED_WITH_TRAPS"})
            self.assertEqual(0, probe["unresolvedSymbols"])
            self.assertTrue(probe["entryPointReached"])
            self.assertGreater(probe["instructions"], 0)
            # The Darwin-only translation layer is registered and reports its
            # real process-stream cells; none of these names is an Android export.
            self.assertEqual(34, probe["darwinCompatBoundSymbols"])
            self.assertEqual(3, probe["darwinCompatStreamCells"])
            # The host probe opts into a finite diagnostic window. The device
            # policy is still unlimited; a real host run may stop at an import,
            # guest/backend fault, or that explicit diagnostic timeout.
            self.assertFalse(probe["executionPolicy"]["unlimited"])
            self.assertEqual(20_000_000, probe["executionPolicy"]["timeLimitMicros"])
            stopped_at_trap = bool(probe["trappedImport"]) and probe["trapCalls"] == 1
            stopped_at_boundary = probe["executionStatus"] in {
                "RETURNED",
                "TIME_LIMIT",
                "INSTRUCTION_LIMIT",
                "GUEST_EXCEPTION_RAISED",
                "MEMORY_FAULT",
                "EXECUTION_FAULT",
                "BACKEND_UNAVAILABLE",
            }
            self.assertTrue(stopped_at_trap or stopped_at_boundary, probe)
            full = json.loads((output / "gameboot-report.json").read_text())
            self.assertEqual("not_runnable", full["status"])
            self.assertTrue(full["trapMode"])


class SplashScreenLauncherTests(unittest.TestCase):
    def test_apk_launchers_render_bundle_splash_screen_instead_of_text_only(self):
        gameboot_java = (
            REPO_ROOT / "gameruntime-template/src/main/java/dev/radek/gameruntime/GameBootActivity.java"
        ).read_text(encoding="utf-8")
        self.assertIn("splashImageView", gameboot_java)
        self.assertIn("parseSplashSheetDescriptor", gameboot_java)
        self.assertIn("decodeCgbiRgbaPixels", gameboot_java)
        self.assertIn("discoverSplashFramesFromBundle", gameboot_java)
        self.assertIn("SPLASHES.dat", gameboot_java)
        self.assertIn("latestGameSurface", gameboot_java)
        self.assertIn("Color.BLACK", gameboot_java)
        self.assertIn("publishGameSurface(latestGameSurface)", gameboot_java)

        placeholder_java = (
            REPO_ROOT
            / "placeholder-template/src/main/java/dev/radek/generated/GeneratedPlaceholderActivity.java"
        ).read_text(encoding="utf-8")
        self.assertIn("readSplash", placeholder_java)
        self.assertIn("splash.png", placeholder_java)

        converted_java = (
            REPO_ROOT / "converted-template/src/main/java/dev/radek/generated/MainActivity.java"
        ).read_text(encoding="utf-8")
        self.assertIn("readSplash", converted_java)
        self.assertIn("Color.BLACK", converted_java)

        with zipfile.ZipFile(ANGRY_BIRDS_IPA) as zf:
            dat = zf.read("Payload/AngryBirds.app/data/SPLASHES.dat")
            png = zf.read("Payload/AngryBirds.app/data/SPLASHES.png")
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        sheet_len = struct.unpack(">H", dat[:2])[0]
        sheet_name = dat[2 : 2 + sheet_len].decode("ascii")
        self.assertEqual("SPLASHES.png", sheet_name)
        entry_count = struct.unpack(">H", dat[2 + sheet_len : 4 + sheet_len])[0]
        self.assertEqual(3, entry_count)


if __name__ == "__main__":
    unittest.main()
