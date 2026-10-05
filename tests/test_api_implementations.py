import ctypes
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

from radek.api_implementations import _SUPPORTED, generate


class MachTimebaseInfo(ctypes.Structure):
    _fields_ = [("numer", ctypes.c_uint32), ("denom", ctypes.c_uint32)]


class ApiImplementationTests(unittest.TestCase):
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

    def test_generates_only_reachable_compiled_compatibility_implementations(self):
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

    @unittest.skipUnless(shutil.which(os.environ.get("CXX", "g++")), "C++ compiler unavailable")
    def test_broad_shims_compile_and_run_when_entry_reachable(self):
        """libc and CoreFoundation shims compile, link and actually execute."""
        reconstruction = self.reconstruction(
            ["_strlen", "_malloc", "_free", "_CFRetain", "_CFRelease", "_CFStringCreateWithCString",
             "_CFStringGetLength"]
        )
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(reconstruction, output)
            source = output / "api-replacements" / "api-replacements.cpp"
            for header in report["headerPaths"]:
                self.assertTrue((output / header).is_file(), header)
            library_path = output / "libshim-broad.so"
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
            strlen = library.radek_compat_strlen
            strlen.argtypes = [ctypes.c_char_p]
            strlen.restype = ctypes.c_size_t
            self.assertEqual(strlen(b"radek"), 5)

            allocate = library.radek_compat_malloc
            allocate.argtypes = [ctypes.c_size_t]
            allocate.restype = ctypes.c_void_p
            block = allocate(128)
            self.assertNotEqual(block, None)
            library.radek_compat_free(ctypes.c_void_p(block))

            create = library.radek_compat_CFStringCreateWithCString
            create.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
            create.restype = ctypes.c_void_p
            text = create(None, b"hello", 0x08000100)
            self.assertNotEqual(text, None)
            length = library.radek_compat_CFStringGetLength
            length.argtypes = [ctypes.c_void_p]
            length.restype = ctypes.c_long
            self.assertEqual(length(ctypes.c_void_p(text)), 5)
            library.radek_compat_CFRelease(ctypes.c_void_p(text))

    def test_android_mapper_table_matches_the_host_compiled_compatibility_table(self):
        kotlin_path = Path(__file__).parents[1] / "app/src/main/java/dev/radek/conventor/AndroidApiMapper.kt"
        source = kotlin_path.read_text(encoding="utf-8")
        start = source.index("private val implementedApiReplacements = mapOf(")
        end = source.index("\n    )", start)
        actual = dict(re.findall(r'"([^"]+)"\s+to\s+"([^"]+)"', source[start:end]))
        expected = {symbol: implementation for symbol, (implementation, _macro) in _SUPPORTED.items()}
        self.assertEqual(expected, actual)

    def test_shim_table_matches_the_native_header_macro(self):
        """The Python table must equal RADEK_IOS_SHIM_TABLE in the C++ header.

        native/src/ioscompat_registry.cpp and native/src/jni.cpp expand that
        macro, so any drift would make the host generator, the on-device
        registry and the JNI resolver disagree about what is implemented.
        """
        from radek.api_implementations import _FAMILY, _SUPPORTED

        header = (Path(__file__).resolve().parent.parent / "native/include/radek_ios_shims.h").read_text()
        start = header.index("#define RADEK_IOS_SHIM_TABLE(X)")
        block = header[start:]
        block = block[: block.index("\n#endif")].replace("\\\n", "\n")
        rows = re.findall(r'^\s*X\(\s*"([^"]+)"\s*,\s*(\w+)\s*\)', block, re.M)
        self.assertTrue(rows, "the shim table macro was not found or is empty")
        for darwin, android in rows:
            with self.subTest(darwin=darwin):
                self.assertEqual(_SUPPORTED[darwin][0], android)
                self.assertEqual(_SUPPORTED[darwin][1], "RADEK_API_" + android)
                expected = "cf" if android[len("radek_compat_"):].startswith("CF") else "libc"
                self.assertEqual(_FAMILY[darwin], expected)
        # Every non-time entry of the Python table must come from the macro.
        macro_symbols = {darwin for darwin, _ in rows}
        python_symbols = {name for name, family in _FAMILY.items() if family != "time"}
        self.assertEqual(python_symbols, macro_symbols)
        self.assertEqual(len(_SUPPORTED), len(_FAMILY))
