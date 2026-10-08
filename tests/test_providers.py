"""Tests for the iOS -> Android provider table.

The table exists in two places because both the host CLI and the Android app must
report the same thing: `radek/providers.py` and
`app/src/main/java/dev/radek/conventor/Providers.kt`. One test here parses the
Kotlin source and asserts the two agree, so they cannot drift.
"""

import re
import unittest
from pathlib import Path

from radek import providers
from radek.compat_import_catalog import (
    CONCRETE_DARWIN_COMPAT_IMPORT_COUNT,
    CONCRETE_DARWIN_COMPAT_PROVIDERS,
)

ROOT = Path(__file__).resolve().parent.parent
KOTLIN = ROOT / "app/src/main/java/dev/radek/conventor/Providers.kt"
API_MAPPER_KOTLIN = ROOT / "app/src/main/java/dev/radek/conventor/AndroidApiMapper.kt"
NATIVE_JNI = ROOT / "native/src/jni.cpp"

# Dependency names commonly seen in on-device compatibility reports.
REPORTED = (
    "/System/Library/Frameworks/Foundation.framework/Foundation",
    "/System/Library/Frameworks/OpenGLES.framework/OpenGLES",
    "/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
    "/System/Library/Frameworks/AVFoundation.framework/AVFoundation",
    "/System/Library/Frameworks/MediaPlayer.framework/MediaPlayer",
    "/System/Library/Frameworks/CoreAudio.framework/CoreAudio",
    "/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox",
    "/System/Library/Frameworks/UIKit.framework/UIKit",
    "/System/Library/Frameworks/OpenAL.framework/OpenAL",
    "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
    "/System/Library/Frameworks/CFNetwork.framework/CFNetwork",
    "/System/Library/Frameworks/CoreMedia.framework/CoreMedia",
    "/System/Library/Frameworks/CoreVideo.framework/CoreVideo",
    "/usr/lib/libiconv.2.dylib",
    "/usr/lib/libgcc_s.1.dylib",
)


class ProviderTests(unittest.TestCase):
    def test_every_reported_framework_has_a_graded_provider_entry(self):
        for install_name in REPORTED:
            with self.subTest(install_name=install_name):
                provider = providers.for_install_name(install_name)
                self.assertIsNotNone(provider, install_name)
                self.assertNotEqual(providers.STATUS_BLOCKED, provider.status)
                self.assertTrue(provider.android)
                self.assertTrue(provider.detail)
                self.assertIn(provider.kind, (providers.KIND_LIBRARY, providers.KIND_PLATFORM, providers.KIND_RUNTIME))

    def test_identical_abis_bind_to_android_system_libraries(self):
        for install_name, expected in (
            ("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", "libGLESv2.so"),
            ("/usr/lib/libiconv.2.dylib", "iconv"),
            ("/usr/lib/libsqlite3.dylib", "libsqlite.so"),
            ("/usr/lib/libSystem.B.dylib", "libc.so"),
            ("/usr/lib/libz.1.dylib", "libz.so"),
        ):
            with self.subTest(install_name=install_name):
                provider = providers.for_install_name(install_name)
                self.assertEqual(providers.STATUS_PROVIDED, provider.status)
                self.assertEqual(providers.KIND_LIBRARY, provider.kind)
                self.assertIn(expected, provider.android)

    def test_legacy_libgcc_install_name_maps_only_to_symbol_level_ndk_candidates(self):
        provider = providers.for_install_name("/usr/lib/libgcc_s.1.dylib")
        self.assertIsNotNone(provider)
        self.assertEqual("libgcc_s", provider.framework)
        self.assertEqual(providers.KIND_RUNTIME, provider.kind)
        self.assertEqual(providers.STATUS_CANDIDATE, provider.status)
        self.assertIn("compiler-rt", provider.android)
        self.assertIn("libunwind", provider.android)
        self.assertIn("no libgcc_s.so alias", provider.android)
        self.assertIn("not a loadable-library mapping", provider.detail)
        self.assertIn("NDK compiler-rt builtins", providers.for_symbol("___aeabi_uidiv"))
        self.assertIn("NDK libunwind", providers.for_symbol("__Unwind_Resume"))
        self.assertIn("NDK compiler-rt builtins", providers.for_symbol("___divti3"))
        self.assertIn("NDK compiler-rt builtins", providers.for_symbol("___divdi3"))
        self.assertIn("NDK libunwind", providers.for_symbol("__aeabi_unwind_cpp_pr0"))

    def test_platform_apis_are_only_compatibility_targets_not_implemented_bridges(self):
        for install_name, expected in (
            ("/System/Library/Frameworks/UIKit.framework/UIKit", "android.view"),
            ("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", "AudioTrack"),
            ("/System/Library/Frameworks/OpenAL.framework/OpenAL", "libaaudio.so"),
            ("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", "Choreographer"),
            ("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", "MediaPlayer"),
            ("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", "Canvas"),
            ("/System/Library/Frameworks/CoreVideo.framework/CoreVideo", "AHardwareBuffer"),
            ("/System/Library/Frameworks/CFNetwork.framework/CFNetwork", "HttpURLConnection"),
        ):
            with self.subTest(install_name=install_name):
                self.assertIn(expected, providers.for_install_name(install_name).android)

    def test_apis_without_an_android_contract_stay_blocked(self):
        for install_name in (
            "/System/Library/Frameworks/StoreKit.framework/StoreKit",
            "/System/Library/Frameworks/GameKit.framework/GameKit",
            "/System/Library/Frameworks/AdSupport.framework/AdSupport",
            "/System/Library/Frameworks/CoreLocation.framework/CoreLocation",
            "/usr/lib/libDoesNotExist.dylib",
        ):
            with self.subTest(install_name=install_name):
                self.assertEqual(providers.STATUS_BLOCKED, providers.classify(install_name)["status"])

    def test_longest_suffix_wins(self):
        provider = providers.for_install_name("/System/Library/Frameworks/UIKit.framework/UIKit")
        self.assertEqual("UIKit", provider.framework)

    def test_symbol_providers(self):
        for symbol, expected in (
            ("_glClear", "OpenGL ES"),
            ("_eglSwapBuffers", "EGL"),
            ("_iconv_open", "iconv"),
            ("_sqlite3_open", "libsqlite"),
            ("_objc_msgSend", "message dispatch"),
            ("_dispatch_async", "concurrent"),
            ("_pthread_create", "pthread"),
            ("_mach_absolute_time", "libioscompat.so"),
            ("_CFAbsoluteTimeGetCurrent", "CFAbsoluteTimeGetCurrent"),
            ("_CACurrentMediaTime", "CACurrentMediaTime"),
            ("_mach_timebase_info", "mach_timebase_info"),
            ("_malloc", "libioscompat.so:radek_compat_malloc"),
        ):
            with self.subTest(symbol=symbol):
                provider = providers.for_symbol(symbol)
                self.assertIsNotNone(provider, symbol)
                self.assertIn(expected, provider)

    def test_unknown_lowercase_symbols_are_not_assumed_to_be_bionic(self):
        self.assertIsNone(providers.for_symbol("_unknownAppleFunction"))
        self.assertIsNone(providers.for_symbol("_private_vendor_entry"))

    def test_coverage_is_honest(self):
        self.assertEqual(100, providers.coverage([], []))
        self.assertEqual(
            100,
            providers.coverage(["/System/Library/Frameworks/OpenGLES.framework/OpenGLES"], ["_glClear"]),
        )
        self.assertLess(
            providers.coverage(["/System/Library/Frameworks/StoreKit.framework/StoreKit"], []), 100
        )
        self.assertEqual(0, providers.coverage(["/usr/lib/libUnknown.dylib"], []))

    def test_native_whitelist_covers_every_provided_library(self):
        for provider in providers.TABLE:
            if provider.kind != providers.KIND_LIBRARY or provider.status == providers.STATUS_BLOCKED:
                continue
            for library in re.findall(r"lib[\w+.-]*\.so", provider.android):
                self.assertIn(library, providers.NATIVE_LIBRARIES, library)

    def test_kotlin_table_matches(self):
        """The app and the CLI must never disagree about a provider."""
        source = KOTLIN.read_text()
        entries = re.findall(
            r'Provider\("([^"]+)",\s*"([^"]+)",\s*\n?\s*"([^"]*)"[^,]*,\s*\n?\s*(KIND_\w+),\s*(STATUS_\w+)',
            source,
        )
        kotlin = {name: (framework, android, kind, status) for name, framework, android, kind, status in entries}
        self.assertEqual(len(providers.TABLE), len(kotlin), "provider tables have different sizes")
        for provider in providers.TABLE:
            with self.subTest(install_name=provider.install_name):
                self.assertIn(provider.install_name, kotlin)
                framework, android, kind, status = kotlin[provider.install_name]
                self.assertEqual(provider.framework, framework)
                self.assertEqual(provider.android, android)
                self.assertEqual("KIND_" + {"native-library": "LIBRARY", "platform-api": "PLATFORM", "runtime": "RUNTIME"}[provider.kind], kind)
                status_values = {
                    "STATUS_PROVIDED": providers.STATUS_PROVIDED,
                    "STATUS_COMPATIBILITY": providers.STATUS_COMPATIBILITY,
                    "STATUS_CANDIDATE": providers.STATUS_CANDIDATE,
                    "STATUS_BLOCKED": providers.STATUS_NO_EXECUTION_PATH_YET,
                    "STATUS_NO_EXECUTION_PATH_YET": providers.STATUS_NO_EXECUTION_PATH_YET,
                }
                self.assertEqual(provider.status, status_values[status])

    def test_concrete_darwin_provider_catalog_is_explicit_and_matches_kotlin(self):
        """Darwin-only names must have a typed provider, not a relabelled NDK hit."""
        self.assertEqual(CONCRETE_DARWIN_COMPAT_IMPORT_COUNT, 73)
        self.assertEqual(len(CONCRETE_DARWIN_COMPAT_PROVIDERS), 73)
        kotlin = (ROOT / "app/src/main/java/dev/radek/conventor/CompatImportProviders.kt").read_text(
            encoding="utf-8"
        )
        pairs = dict(re.findall(r'^\s*"((?:[^"\\]|\\.)*)"\s+to\s+"([^"]+)"', kotlin, re.M))
        # Kotlin escapes the dollar in Objective-C class names inside string
        # templates; source-level parity restores it for comparison.
        pairs = {name.replace(r"\$", "$" ): provider for name, provider in pairs.items()}
        self.assertEqual(CONCRETE_DARWIN_COMPAT_PROVIDERS, pairs)
        for symbol, provider in CONCRETE_DARWIN_COMPAT_PROVIDERS.items():
            with self.subTest(symbol=symbol):
                self.assertEqual(providers.CONCRETE_DARWIN_COMPAT_PROVIDERS[symbol], "libioscompat.so:" + provider)
                self.assertIn(provider, providers.for_symbol(symbol))

    def test_ndk_name_candidate_catalogs_match_between_kotlin_and_host(self):
        """The on-device mapper and the host CLI must agree on every candidate.

        A symbol listed only on one side would make the device report a direct
        NDK candidate that the host calls unmapped (or the other way round).
        """
        source = API_MAPPER_KOTLIN.read_text(encoding="utf-8")
        block = source[source.index("private val bionicLibraries = mapOf("):]
        block = block[: block.index("\n    )")]
        per_library = {
            library: set(re.findall(r'"([^"]+)"', chunk))
            for library, chunk in re.findall(r'"(lib[^"]+\.so)" to setOf\((.*?)\n        \)', block, re.S)
        }
        self.assertTrue(per_library, "failed to parse the Kotlin NDK candidate catalog")
        self.assertEqual(set(providers.BIONIC_SYMBOL_CANDIDATES), set().union(*per_library.values()))
        # Candidates are shared names only: the Kotlin catalog must never list one
        # symbol in two libraries, or the reported target library would depend on
        # map iteration order.
        all_symbols = [symbol for symbols in per_library.values() for symbol in symbols]
        self.assertEqual(len(all_symbols), len(set(all_symbols)))
        for expected in ("malloc", "memcpy", "glDrawArrays", "glAlphaFunc", "deflate", "eglSwapBuffers", "dlopen"):
            self.assertIn(expected, providers.BIONIC_SYMBOL_CANDIDATES)
        # OpenAL has no Android provider, so its entry points must stay out of the
        # same-name catalog and keep reaching the compat-stub resolver instead.
        openal = {"alSourcePlay", "alDeleteSources", "alGenBuffers", "alSourceQueueBuffers", "alcOpenDevice"}
        self.assertFalse(openal & providers.BIONIC_SYMBOL_CANDIDATES)

    def test_runtime_ndk_export_whitelists_match_between_kotlin_and_jni(self):
        kotlin = API_MAPPER_KOTLIN.read_text()
        cpp = NATIVE_JNI.read_text()
        kotlin_block = kotlin.split("private val ndkRuntimeLibraries = setOf(", 1)[1].split(")", 1)[0]
        cpp_block = cpp.split("kPublicNdkLibraries[] = {", 1)[1].split("};", 1)[0]
        kotlin_libraries = set(re.findall(r'"(lib[^"]+\.so)"', kotlin_block))
        cpp_libraries = set(re.findall(r'"(lib[^"]+\.so)"', cpp_block))
        self.assertEqual(kotlin_libraries, cpp_libraries)
        self.assertTrue({"libnativewindow.so", "libneuralnetworks.so", "libsync.so"}.issubset(kotlin_libraries))

    def test_analysis_edges_carry_graded_status_and_fail_closed_evidence(self):
        edge = providers.classify("/System/Library/Frameworks/UIKit.framework/UIKit")
        self.assertEqual(edge["status"], providers.STATUS_CANDIDATE)
        self.assertEqual(edge["classification"], providers.STATUS_CANDIDATE)
        self.assertEqual(edge["providerKind"], providers.KIND_PLATFORM)
        evidence = edge["evidence"]
        self.assertEqual(evidence["observedImportCount"], 0)
        self.assertEqual(evidence["exportsVerifiedOnThisDevice"], 0)
        self.assertEqual(evidence["hostTestedImplementations"], 0)
        self.assertEqual(evidence["stubOnlyCount"], 0)
        self.assertTrue(evidence["none"])
        self.assertFalse(evidence["runtimeBackingClaimed"])
        self.assertEqual(evidence["linkedGameCallCount"], 0)
        self.assertFalse(evidence["runtimeCallsObserved"])
        self.assertEqual(evidence["recompiledBytesLinked"], 0)

    def test_required_grade_corrections_and_partial_frameworks(self):
        expected = {
            "libgcc_s.1.dylib": providers.STATUS_CANDIDATE,
            "libstdc++.6.dylib": providers.STATUS_CANDIDATE,
            "CoreFoundation.framework/CoreFoundation": providers.STATUS_COMPATIBILITY,
            "QuartzCore.framework/QuartzCore": providers.STATUS_COMPATIBILITY,
            "Foundation.framework/Foundation": providers.STATUS_CANDIDATE,
            "UIKit.framework/UIKit": providers.STATUS_CANDIDATE,
            "CoreGraphics.framework/CoreGraphics": providers.STATUS_CANDIDATE,
            "OpenAL.framework/OpenAL": providers.STATUS_CANDIDATE,
            "AudioToolbox.framework/AudioToolbox": providers.STATUS_CANDIDATE,
        }
        for suffix, grade in expected.items():
            with self.subTest(install_name=suffix):
                provider = providers.for_install_name(suffix)
                self.assertIsNotNone(provider)
                self.assertEqual(grade, provider.status)
        stdcxx = providers.for_install_name("/usr/lib/libstdc++.6.dylib")
        self.assertIn("GNU libstdc++", stdcxx.detail)
        self.assertIn("different C++ ABIs and mangling", stdcxx.detail)
        self.assertNotIn("NDK libc++_shared.so (NDK)", stdcxx.android)
        for suffix in ("CoreFoundation.framework/CoreFoundation", "QuartzCore.framework/QuartzCore"):
            provider = providers.for_install_name(suffix)
            self.assertNotEqual(providers.STATUS_PROVIDED, provider.status)
            self.assertIn("Partial only", provider.detail)

    def test_per_import_evidence_keeps_host_tests_stubs_and_none_separate(self):
        evidence = providers.evidence_summary(
            ["_CFRelease", "_objc_msgSend", "_unmapped"],
            verified_device_symbols=["_CFRelease"],
            host_tested_symbols=["_CFRelease"],
            stub_only_symbols=["_objc_msgSend"],
        )
        self.assertEqual(evidence["observedImportCount"], 3)
        self.assertEqual(evidence["exportsVerifiedOnThisDevice"], 1)
        self.assertEqual(evidence["hostTestedImplementations"], 1)
        self.assertEqual(evidence["stubOnlyCount"], 1)
        self.assertEqual(evidence["noneCount"], 1)
        self.assertFalse(evidence["none"])
        self.assertIn("none", evidence["evidenceKinds"])
        self.assertFalse(evidence["runtimeBackingClaimed"])
        self.assertEqual(evidence["linkedGameCallCount"], 0)


if __name__ == "__main__":
    unittest.main()
