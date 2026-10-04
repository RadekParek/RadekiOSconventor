import ctypes
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from radek.compat_layer import CONTRACT, classify, collect_imports, generate


def reconstruction(imports):
    return {
        "images": [
            {
                "slices": [
                    {"imports": [{"name": name} for name in imports]},
                ]
            }
        ]
    }


class CompatLayerTests(unittest.TestCase):
    def test_collects_and_deduplicates_imports(self):
        data = {
            "images": [
                {"slices": [{"imports": [{"name": "_b"}, "_a"]}]},
                {"slices": [{"imports": [{"name": "_a"}, {"name": "  _c  "}]}]},
            ]
        }
        self.assertEqual(collect_imports(data), ["_a", "_b", "_c"])
        self.assertEqual(collect_imports({"images": []}), [])
        self.assertEqual(classify("_CFAbsoluteTimeGetCurrent"), "verified")
        self.assertEqual(classify("_glDrawArrays"), "stubbed")

    def test_no_imports_reports_zero_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            report = generate({"images": [{"slices": [{}]}]}, Path(directory))
            self.assertEqual(report["status"], "NO_OBSERVED_IMPORTS")
            self.assertEqual(report["handlerResolutionCoveragePercent"], 0)

    def test_generates_verified_and_stubbed_registry(self):
        imports = [
            "_CFAbsoluteTimeGetCurrent",
            "_mach_absolute_time",
            "_glDrawArrays",
            "_OBJC_CLASS_$_UIView",
            "_alcOpenDevice",
        ]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(reconstruction(imports), output)
            self.assertEqual(report["status"], "REGISTRY_SOURCE_GENERATED")
            self.assertEqual(report["contract"], CONTRACT)
            self.assertEqual(report["verifiedImplementations"], 2)
            self.assertEqual(report["stubbedHandlers"], 3)
            self.assertEqual(report["totalObservedImports"], 5)
            self.assertEqual(report["unresolvedImports"], 0)
            self.assertEqual(report["handlerResolutionCoveragePercent"], 100)
            self.assertFalse(report["completeGameConversion"])
            source = (output / "ioscompat" / "libioscompat.cpp").read_text()
            self.assertIn("NOT implementations", source)
            self.assertIn('"_glDrawArrays"', source)
            self.assertIn('"CFAbsoluteTimeGetCurrent"', source)
            registry = json.loads((output / "ioscompat" / "registry.json").read_text())
            self.assertEqual(registry["verifiedImplementations"], 2)
            by_name = {entry["sourceSymbol"]: entry for entry in registry["entries"]}
            self.assertEqual(
                by_name["_CFAbsoluteTimeGetCurrent"]["classification"], "verified"
            )
            self.assertTrue(by_name["_CFAbsoluteTimeGetCurrent"]["implementationPresent"])
            self.assertEqual(by_name["_glDrawArrays"]["classification"], "stubbed-unimplemented")
            self.assertFalse(by_name["_glDrawArrays"]["implementationPresent"])
            self._assert_source_compiles_and_resolves(output / "ioscompat")

    @staticmethod
    def _compile(source_dir: Path, directory: Path) -> Path:
        library = Path(directory) / "libioscompat.so"
        subprocess.run(
            [
                os.environ.get("CXX", "g++"),
                "-std=c++17",
                "-shared",
                "-fPIC",
                "-pthread",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(source_dir),
                str(source_dir / "libioscompat.cpp"),
                "-o",
                str(library),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        return library

    @unittest.skipUnless(shutil.which(os.environ.get("CXX", "g++")), "C++ compiler unavailable")
    def _assert_source_compiles_and_resolves(self, source_dir: Path):
        with tempfile.TemporaryDirectory() as directory:
            library = self._compile(source_dir, Path(directory))
            lib = ctypes.CDLL(str(library))
            lib.radek_compat_generated_classify.restype = ctypes.c_char_p
            lib.radek_compat_generated_resolve.restype = ctypes.c_void_p
            lib.radek_compat_generated_invoke_stub.restype = ctypes.c_longlong
            lib.radek_compat_generated_stub_call_total.restype = ctypes.c_ulonglong
            lib.radek_compat_generated_entry_count.restype = ctypes.c_ulong

            # Verified symbols resolve to the real implementation bodies.
            self.assertEqual(
                lib.radek_compat_generated_classify(b"_CFAbsoluteTimeGetCurrent"), b"verified"
            )
            self.assertNotEqual(lib.radek_compat_generated_resolve(b"_mach_absolute_time"), None)

            # Everything else resolves to explicit stub handlers, never "verified".
            self.assertEqual(lib.radek_compat_generated_classify(b"_glDrawArrays"), b"stubbed")
            self.assertEqual(lib.radek_compat_generated_classify(b"_unknown_symbol"), None)
            self.assertNotEqual(lib.radek_compat_generated_resolve(b"_glDrawArrays"), None)

            # Invoking a stub is safe, observable, and documented-zero.
            self.assertEqual(lib.radek_compat_generated_invoke_stub(b"_glDrawArrays"), 0)
            self.assertEqual(lib.radek_compat_generated_invoke_stub(b"_glDrawArrays"), 0)
            self.assertEqual(lib.radek_compat_generated_invoke_stub(b"_alcOpenDevice"), 0)
            self.assertEqual(lib.radek_compat_generated_stub_call_total(), 3)
            self.assertEqual(lib.radek_compat_generated_invoke_stub(b"_CFAbsoluteTimeGetCurrent"), -1)
            self.assertEqual(lib.radek_compat_generated_invoke_stub(b"_unknown_symbol"), -1)

            # Enumeration matches the report's honest split.
            self.assertEqual(lib.radek_compat_generated_entry_count(), 5)
            darwin = ctypes.c_char_p()
            android = ctypes.c_char_p()
            kind = ctypes.c_int()
            lib.radek_compat_generated_entry_at.argtypes = [
                ctypes.c_ulong,
                ctypes.POINTER(ctypes.c_char_p),
                ctypes.POINTER(ctypes.c_char_p),
                ctypes.POINTER(ctypes.c_int),
            ]
            self.assertEqual(
                lib.radek_compat_generated_entry_at(0, darwin, android, kind), 0
            )
            self.assertIn(kind.value, (1, 2))
            self.assertEqual(
                lib.radek_compat_generated_entry_at(5, darwin, android, kind), -1
            )

    @unittest.skipUnless(shutil.which(os.environ.get("CXX", "g++")), "C++ compiler unavailable")
    def test_large_registry_seeds_across_multiple_template_chunks(self):
        # clang rejects folds wider than its nesting limit; the generated source
        # must stay compilable well beyond one 128-wide chunk.
        imports = [f"_radek_generated_symbol_{index}" for index in range(300)]
        imports.append("_CFAbsoluteTimeGetCurrent")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(reconstruction(imports), output)
            self.assertEqual(report["stubbedHandlers"], 300)
            library = self._compile(output / "ioscompat", Path(directory))
            lib = ctypes.CDLL(str(library))
            lib.radek_compat_generated_classify.restype = ctypes.c_char_p
            lib.radek_compat_generated_entry_count.restype = ctypes.c_ulong
            lib.radek_compat_generated_invoke_stub.restype = ctypes.c_longlong
            lib.radek_compat_generated_stub_call_total.restype = ctypes.c_ulonglong
            self.assertEqual(lib.radek_compat_generated_entry_count(), 301)
            self.assertEqual(
                lib.radek_compat_generated_classify(b"_radek_generated_symbol_299"), b"stubbed"
            )
            self.assertEqual(
                lib.radek_compat_generated_classify(b"_CFAbsoluteTimeGetCurrent"), b"verified"
            )
            # A stub from the last seeded chunk resolves and records its call.
            self.assertEqual(
                lib.radek_compat_generated_invoke_stub(b"_radek_generated_symbol_299"), 0
            )
            self.assertEqual(lib.radek_compat_generated_stub_call_total(), 1)

    def test_unsafe_symbol_names_are_never_embedded(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(reconstruction(['_bad"name', "_good_symbol"]), output)
            source = (output / "ioscompat" / "libioscompat.cpp").read_text()
        self.assertEqual(report["status"], "REGISTRY_SOURCE_GENERATED")
        self.assertEqual(report["stubbedHandlers"], 1)
        self.assertEqual(report["unresolvedImports"], 1)
        self.assertEqual(report["rejectedUnsafeNames"], 1)
        self.assertEqual(report["handlerResolutionCoveragePercent"], 50.0)
        self.assertIn("_good_symbol", source)
        self.assertNotIn("_bad", source)


if __name__ == "__main__":
    unittest.main()

    def test_broad_shims_are_classified_verified_with_their_family(self):
        from radek.compat_layer import family

        self.assertEqual(classify("_strlen"), "verified")
        self.assertEqual(classify("_memcpy"), "verified")
        self.assertEqual(classify("_pthread_mutex_lock"), "verified")
        self.assertEqual(classify("_CFStringCreateWithCString"), "verified")
        self.assertEqual(classify("_CFDictionarySetValue"), "verified")
        self.assertEqual(family("_strlen"), "libc")
        self.assertEqual(family("_CFRetain"), "cf")
        self.assertEqual(family("_CFAbsoluteTimeGetCurrent"), "time")
        self.assertEqual(family("_glDrawArrays"), "")
        # Unimplemented APIs must never be promoted to verified.
        self.assertEqual(classify("_UIApplicationMain"), "stubbed")
        self.assertEqual(classify("_objc_msgSend"), "stubbed")
        self.assertEqual(classify("_glDrawArrays"), "stubbed")

    @unittest.skipUnless(shutil.which(os.environ.get("CXX", "g++")), "C++ compiler unavailable")
    def test_generated_source_builds_and_runs_broad_shims(self):
        imports = ["_strlen", "_CFStringCreateWithCString", "_CFStringGetLength", "_glDrawArrays"]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(reconstruction(imports), output)
            self.assertEqual(report["verifiedImplementations"], 3)
            self.assertEqual(report["stubbedHandlers"], 1)
            for header in report["headerPaths"]:
                self.assertTrue((output / header).is_file(), header)
            library = self._compile(output / "ioscompat", Path(directory))

            classify_fn = library.radek_compat_generated_classify
            classify_fn.argtypes = [ctypes.c_char_p]
            classify_fn.restype = ctypes.c_char_p
            self.assertEqual(classify_fn(b"_strlen"), b"verified")
            self.assertEqual(classify_fn(b"_CFStringGetLength"), b"verified")
            self.assertEqual(classify_fn(b"_glDrawArrays"), b"stubbed")

            strlen = library.radek_compat_strlen
            strlen.argtypes = [ctypes.c_char_p]
            strlen.restype = ctypes.c_size_t
            self.assertEqual(strlen(b"radek"), 5)

            create = library.radek_compat_CFStringCreateWithCString
            create.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
            create.restype = ctypes.c_void_p
            length = library.radek_compat_CFStringGetLength
            length.argtypes = [ctypes.c_void_p]
            length.restype = ctypes.c_long
            text = create(None, b"hello", 0x08000100)
            self.assertNotEqual(text, None)
            self.assertEqual(length(ctypes.c_void_p(text)), 5)
            library.radek_compat_CFRelease(ctypes.c_void_p(text))
