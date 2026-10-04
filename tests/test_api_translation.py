import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

from radek.api_translation import generate


class MachTimebaseInfo(ctypes.Structure):
    _fields_ = [("numer", ctypes.c_uint32), ("denom", ctypes.c_uint32)]


class ApiTranslationTests(unittest.TestCase):
    @staticmethod
    def reconstruction(used, callers=None):
        caller_names = callers or ["_main"]
        return {
            "images": [
                {
                    "slices": [
                        {
                            "entryPoint": "0x1000",
                            "functions": [{"name": "_main", "address": "0x1000"}],
                            "callGraph": {"edges": []},
                            "apis": {
                                "used": [
                                    {"name": name, "callers": caller_names}
                                    for name in used
                                ],
                                "unused": [],
                            },
                        },
                    ]
                }
            ]
        }

    def test_generates_only_reachable_implemented_time_api_wrappers(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(
                self.reconstruction(
                    [
                        "_CFAbsoluteTimeGetCurrent",
                        "_CACurrentMediaTime",
                        "_mach_absolute_time",
                        "_mach_timebase_info",
                        "_UIApplicationMain",
                    ]
                ),
                output,
            )

            self.assertEqual(report["status"], "IMPLEMENTATIONS_GENERATED_NOT_LINKED")
            self.assertEqual(report["generatedApiReplacements"], 4)
            self.assertEqual(report["linkedApiReplacements"], 0)
            self.assertTrue(report["codeGenerated"])
            self.assertFalse(report["completeGameConversion"])
            self.assertEqual(len(report["replacements"]), 4)
            self.assertTrue(all(item["codeGenerated"] for item in report["replacements"]))
            self.assertTrue(all(not item["linkedIntoGame"] for item in report["replacements"]))
            self.assertTrue(all(item["reachableFromEntry"] for item in report["replacements"]))
            source_dir = output / "api-replacements"
            source = (source_dir / "api-replacements.cpp").read_text()
            self.assertIn("#define RADEK_API_REPLACEMENTS_ONLY 1", source)
            self.assertIn("#define RADEK_API_CFAbsoluteTimeGetCurrent 1", source)
            self.assertTrue((source_dir / "apple_time_compat.h").is_file())
            self.assertIn("CLOCK_REALTIME", source)
            self.assertIn("extern \"C\" uint64_t mach_absolute_time", source)

    @unittest.skipUnless(shutil.which(os.environ.get("CXX", "g++")), "C++ compiler unavailable")
    def test_generated_compatibility_code_executes_real_clock_replacements(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            generate(
                self.reconstruction(
                    [
                        "_CFAbsoluteTimeGetCurrent",
                        "_CACurrentMediaTime",
                        "_mach_absolute_time",
                        "_mach_timebase_info",
                    ]
                ),
                output,
            )
            source = output / "api-replacements" / "api-replacements.cpp"
            library_path = output / "libapi-replacements.so"
            subprocess.run(
                [
                    os.environ.get("CXX", "g++"),
                    "-std=c++17",
                    "-shared",
                    "-fPIC",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(source),
                    "-o",
                    str(library_path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            library = ctypes.CDLL(str(library_path))
            absolute = library.CFAbsoluteTimeGetCurrent
            absolute.restype = ctypes.c_double
            expected = time.time() - 978307200.0
            self.assertLess(abs(absolute() - expected), 2.0)

            media = library.CACurrentMediaTime
            media.restype = ctypes.c_double
            media_before = media()
            ticks = library.mach_absolute_time
            ticks.restype = ctypes.c_uint64
            ticks_before = ticks()
            time.sleep(0.002)
            media_after = media()
            ticks_after = ticks()
            self.assertGreater(media_after, media_before)
            self.assertGreater(ticks_after, ticks_before)

            timebase = library.mach_timebase_info
            timebase.argtypes = [ctypes.POINTER(MachTimebaseInfo)]
            timebase.restype = ctypes.c_int32
            info = MachTimebaseInfo()
            self.assertEqual(timebase(ctypes.byref(info)), 0)
            self.assertEqual((info.numer, info.denom), (1, 1))
            self.assertEqual(timebase(None), 4)

    @unittest.skipUnless(shutil.which(os.environ.get("CXX", "g++")), "C++ compiler unavailable")
    def test_each_single_api_shim_compiles_without_unused_helper_warnings(self):
        for symbol in (
            "_CFAbsoluteTimeGetCurrent",
            "_CACurrentMediaTime",
            "_mach_absolute_time",
            "_mach_timebase_info",
        ):
            with self.subTest(symbol=symbol), tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                generate(self.reconstruction([symbol]), output)
                source = output / "api-replacements" / "api-replacements.cpp"
                subprocess.run(
                    [
                        os.environ.get("CXX", "g++"),
                        "-std=c++17",
                        "-shared",
                        "-fPIC",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        str(source),
                        "-o",
                        str(output / "libshim.so"),
                    ],
                    check=True,
                    capture_output=True,
                    text=True,
                )

    def test_entry_reachability_follows_only_resolved_internal_calls(self):
        reconstruction = self.reconstruction(
            ["_CFAbsoluteTimeGetCurrent"],
            callers=["-[Clock now]"],
        )
        slice_data = reconstruction["images"][0]["slices"][0]
        slice_data["functions"].append({"name": "-[Clock now]", "address": "0x2000"})
        slice_data["callGraph"]["edges"].append(
            {"from": "_main", "to": "-[Clock now]", "address": "0x2000", "external": False}
        )
        with tempfile.TemporaryDirectory() as directory:
            report = generate(reconstruction, Path(directory))
        self.assertEqual(report["generatedApiReplacements"], 1)
        self.assertTrue(report["replacements"][0]["reachableFromEntry"])

    def test_unreachable_and_unsupported_imports_do_not_generate_shims(self):
        reconstruction = self.reconstruction(
            ["_UIApplicationMain", "_CFAbsoluteTimeGetCurrent"],
            callers=["unreachableFunction"],
        )
        with tempfile.TemporaryDirectory() as directory:
            report = generate(reconstruction, Path(directory))
        self.assertEqual(report["generatedApiReplacements"], 0)
        self.assertFalse(report["codeGenerated"])
        self.assertEqual(report["status"], "NO_ENTRY_REACHABLE_IMPLEMENTED_API")
