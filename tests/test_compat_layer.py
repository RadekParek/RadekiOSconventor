import ctypes
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

from radek.compat_layer import CONTRACT, classify, collect_imports, generate
from radek.compat_import_catalog import CONCRETE_DARWIN_COMPAT_PROVIDERS
from radek.game import macho
from radek.providers import BIONIC_SYMBOL_CANDIDATES


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
        self.assertEqual(classify("_glDrawArrays"), "verified")
        self.assertEqual(classify("_UnknownPrivateGameSymbol"), "stubbed")

    def test_no_imports_reports_zero_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            report = generate({"images": [{"slices": [{}]}]}, Path(directory))
            self.assertEqual(report["status"], "NO_OBSERVED_IMPORTS")
            self.assertEqual(report["handlerResolutionCoveragePercent"], 0)
            self.assertEqual(report["guestRuntimeAdapterCatalogCount"], 0)
            self.assertEqual(report["guestRuntimeSlotFixupsStatus"], "NOT_RUN")
            self.assertEqual(report["staticGameCallsitesRewritten"], 0)

    def test_angry_birds_provider_ledger_is_181_ndk_plus_73_catalog_matches(self):
        """The exact catalogs partition the fixture without implying runtime links.

        All 73 catalog-matched imports resolve to adapters whose semantics are
        implemented and pinned by the native compat-runtime test suite, so the
        outstanding-adapter count is zero.
        """
        native_catalog = (
            Path(__file__).resolve().parent.parent
            / "native/include/compat_runtime/ndk_import_catalog.hpp"
        ).read_text(encoding="utf-8")
        ndk_names = re.findall(r'\{"([^"]+)"\}', native_catalog)
        self.assertEqual(len(ndk_names), 181)
        self.assertEqual(len(set(ndk_names)), 181)
        fixture = Path(__file__).resolve().parent / "data" / "AngryBirds_v1.0_os30.ipa"
        with zipfile.ZipFile(fixture) as archive:
            image = macho.parse(archive.read("Payload/AngryBirds.app/AngryBirds"))
        imports = sorted({symbol.name for symbol in image.undefined_symbols if symbol.name})
        self.assertEqual(len(imports), 254)
        strict_ndk = set(imports) & set(ndk_names)
        guest_catalog = set(imports) & set(CONCRETE_DARWIN_COMPAT_PROVIDERS)
        self.assertEqual(len(strict_ndk), 181)
        self.assertEqual(len(guest_catalog), 73)
        self.assertEqual(strict_ndk | guest_catalog, set(imports))
        self.assertFalse(strict_ndk & guest_catalog)
        with tempfile.TemporaryDirectory() as directory:
            report = generate(reconstruction(imports), Path(directory))
        self.assertEqual(report["totalObservedImports"], 254)
        self.assertEqual(report["sameNameNdkCandidateCount"], 181)
        self.assertEqual(report["sameNameNdkProviderCount"], 181)
        self.assertEqual(report["concreteDarwinProviderCount"], 73)  # legacy alias: all catalog matches
        self.assertEqual(report["guestRuntimeAdapterCatalogCount"], 0)  # all catalog matches now verified
        self.assertEqual(report["verifiedGuestAdapterCount"], 73)  # verified-semantics promotion
        self.assertEqual(report["guestRuntimeAdapterCatalogInventoryCount"], 75)
        self.assertAlmostEqual(report["guestRuntimeAdapterCatalogCoveragePercent"], 28.7402)
        self.assertEqual(report["guestRuntimeAdapterCatalogStatus"], "CATALOG_ONLY_NOT_RUNTIME_LINKED")
        self.assertEqual(report["guestRuntimeSlotFixupsStatus"], "NOT_RUN")
        self.assertEqual(report["staticGameCallsitesRewritten"], 0)
        self.assertEqual(report["reviewedImportProviderCount"], 254)
        self.assertEqual(report["importProviderCoveragePercent"], 100.0)
        self.assertEqual(report["importProviderStatus"], "COMPLETE")

    def test_full_ndk_catalog_matches_host_candidate_inventory(self):
        native_catalog = (
            Path(__file__).resolve().parent.parent
            / "native/include/compat_runtime/ndk_full_import_catalog.hpp"
        ).read_text(encoding="utf-8")
        native_names = {
            name
            for name in re.findall(r'\{"([^\"]+)",\s*"[^\"]+"\}', native_catalog)
        }
        self.assertEqual(len(BIONIC_SYMBOL_CANDIDATES), 1229)
        self.assertEqual(native_names, {"_" + name for name in BIONIC_SYMBOL_CANDIDATES})

    def test_generates_verified_and_stubbed_registry(self):
        imports = [
            "_CFAbsoluteTimeGetCurrent",
            "_mach_absolute_time",
            "_UnknownPrivateGameSymbol",
            "_CustomUnmappedSymbol1",
            "_CustomUnmappedSymbol2",
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
            self.assertIn('"_UnknownPrivateGameSymbol"', source)
            self.assertIn('"CFAbsoluteTimeGetCurrent"', source)
            registry = json.loads((output / "ioscompat" / "registry.json").read_text())
            self.assertEqual(registry["verifiedImplementations"], 2)
            by_name = {entry["sourceSymbol"]: entry for entry in registry["entries"]}
            self.assertEqual(
                by_name["_CFAbsoluteTimeGetCurrent"]["classification"], "verified"
            )
            self.assertTrue(by_name["_CFAbsoluteTimeGetCurrent"]["implementationPresent"])
            self.assertEqual(by_name["_UnknownPrivateGameSymbol"]["classification"], "stubbed-unimplemented")
            self.assertFalse(by_name["_UnknownPrivateGameSymbol"]["implementationPresent"])
            self._assert_source_compiles_and_resolves(output / "ioscompat")

    def test_every_observed_import_is_classified_exactly_once(self):
        imports = [
            "_CFAbsoluteTimeGetCurrent",
            "_mach_absolute_time",
            "_glDrawArrays",
            "_OBJC_CLASS_$_UIView",
            "_alcOpenDevice",
            "_NSLog",
            "_dispatch_async",
            "_strlen",
            "_notify_post",
        ]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(reconstruction(imports), output)
            registry = json.loads((output / "ioscompat" / "registry.json").read_text())
            entries = registry["entries"]
            observed = collect_imports(reconstruction(imports))

            # Resolution is total: every observed import has exactly one target.
            names = [entry["sourceSymbol"] for entry in entries]
            self.assertEqual(sorted(names), sorted(observed))
            self.assertEqual(len(names), len(set(names)))
            by_name = {entry["sourceSymbol"]: entry for entry in entries}
            guest_entry = by_name["_OBJC_CLASS_$_UIView"]
            self.assertEqual(guest_entry["providerKind"], "guest-runtime-adapter-catalogued")
            self.assertEqual(guest_entry["guestRuntimeProviderLibrary"], "libcompat_runtime_v1.so")
            self.assertIsNotNone(guest_entry["guestRuntimeProvider"])
            self.assertFalse(guest_entry["staticGameCallsiteRewritten"])
            self.assertEqual(report["guestRuntimeSlotFixupsStatus"], "NOT_RUN")
            for entry in entries:
                self.assertIn(entry["classification"], ("verified", "stubbed-unimplemented"))
                self.assertEqual(
                    entry["implementationPresent"], entry["classification"] == "verified"
                )

            # The accounting balances: implementations + stubs + unresolved == observed.
            self.assertEqual(
                registry["verifiedImplementations"]
                + registry["stubbedHandlers"]
                + registry["unresolvedImports"],
                registry["totalObservedImports"],
            )
            self.assertEqual(report["handlerResolutionCoveragePercent"], 100)

            # Total resolution coverage is still not implementation coverage: the
            # host-tested implementations are a minority of the registry and no
            # stage of this report claims an APK or a linked game.
            expected_verified = sum(1 for name in observed if classify(name) == "verified")
            self.assertEqual(report["verifiedImplementations"], expected_verified)
            self.assertGreater(expected_verified, 0)
            resolution = report["symbolResolution"]
            self.assertEqual(resolution["status"], "COMPLETE")
            self.assertEqual(resolution["verifiedImplementations"], expected_verified)
            self.assertEqual(
                resolution["verifiedImplementations"]
                + resolution["stubbedUnimplemented"]
                + resolution["unresolved"],
                resolution["total"],
            )
            self.assertEqual(resolution["linkedIntoGame"], 0)
            self.assertTrue(resolution["resolutionIsNotImplementation"])
            self.assertLess(report["verifiedImplementations"], registry["totalObservedImports"])
            self.assertFalse(report["completeGameConversion"])
            self.assertIn("resolution target", report["message"])

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
            self.assertEqual(lib.radek_compat_generated_classify(b"_UnknownPrivateGameSymbol"), b"stubbed")
            self.assertEqual(lib.radek_compat_generated_classify(b"_unknown_symbol"), None)
            self.assertNotEqual(lib.radek_compat_generated_resolve(b"_UnknownPrivateGameSymbol"), None)

            # Invoking a stub is safe, observable, and documented-zero.
            self.assertEqual(lib.radek_compat_generated_invoke_stub(b"_UnknownPrivateGameSymbol"), 0)
            self.assertEqual(lib.radek_compat_generated_invoke_stub(b"_UnknownPrivateGameSymbol"), 0)
            self.assertEqual(lib.radek_compat_generated_invoke_stub(b"_CustomUnmappedSymbol1"), 0)
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
        self.assertEqual(family("_glDrawArrays"), "libc")
        self.assertEqual(classify("_UIApplicationMain"), "verified")
        self.assertEqual(classify("_objc_msgSend"), "verified")
        self.assertEqual(classify("_glDrawArrays"), "verified")
        # Unimplemented APIs must never be promoted to verified.
        self.assertEqual(family("_UnknownPrivateGameSymbol"), "")
        self.assertEqual(classify("_UnknownPrivateGameSymbol"), "stubbed")

    @unittest.skipUnless(shutil.which(os.environ.get("CXX", "g++")), "C++ compiler unavailable")
    def test_generated_source_builds_and_runs_broad_shims(self):
        imports = [
            "_strlen",
            "_CFStringCreateWithCString",
            "_CFStringGetLength",
            "_CFRunLoopGetCurrent",
            "_CFRunLoopRunInMode",
            "_UnknownPrivateGameSymbol",
        ]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(reconstruction(imports), output)
            self.assertEqual(report["verifiedImplementations"], 5)
            self.assertEqual(report["stubbedHandlers"], 1)
            for header in report["headerPaths"]:
                self.assertTrue((output / header).is_file(), header)
            library = self._compile(output / "ioscompat", Path(directory))

            classify_fn = library.radek_compat_generated_classify
            classify_fn.argtypes = [ctypes.c_char_p]
            classify_fn.restype = ctypes.c_char_p
            self.assertEqual(classify_fn(b"_strlen"), b"verified")
            self.assertEqual(classify_fn(b"_CFStringGetLength"), b"verified")
            self.assertEqual(classify_fn(b"_UnknownPrivateGameSymbol"), b"stubbed")

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

            run_loop = library.radek_compat_CFRunLoopGetCurrent
            run_loop.restype = ctypes.c_void_p
            current = run_loop()
            mode = create(None, b"default", 0x08000100)
            callback_type = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
            callback_count = ctypes.c_int(0)

            @callback_type
            def on_run_loop(context):
                ctypes.cast(context, ctypes.POINTER(ctypes.c_int)).contents.value += 1

            perform = library.radek_compat_CFRunLoopPerform
            perform.argtypes = [ctypes.c_void_p, ctypes.c_void_p, callback_type, ctypes.c_void_p]
            perform.restype = ctypes.c_ubyte
            self.assertEqual(perform(current, mode, on_run_loop, ctypes.byref(callback_count)), 1)
            run_in_mode = library.radek_compat_CFRunLoopRunInMode
            run_in_mode.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_ubyte]
            run_in_mode.restype = ctypes.c_int32
            self.assertEqual(run_in_mode(mode, 1.0, 1), 4)
            self.assertEqual(callback_count.value, 1)
            library.radek_compat_CFRelease(ctypes.c_void_p(mode))
