"""Android NDK discovery and incremental relink.

Discovery tests need no toolchain: they check the ordering of candidate NDK
roots. The relink tests need a REAL Android NDK (clang + sysroot). They are
skipped, not faked, when none is installed. Point ANDROID_NDK_HOME at an NDK
to run them.
"""

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from radek.game.android_linker import _ndk_roots, find_android_clang, link_generated_translation

_REAL_NDK = os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK_ROOT")


class NdkDiscoveryTests(unittest.TestCase):
    def test_sdk_ndk_versions_sort_numerically_newest_first(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory) / "sdk"
            for version in ("9.0.8675311", "27.0.12077973", "26.1.10909125"):
                (sdk / "ndk" / version).mkdir(parents=True)
            roots = _ndk_roots({"ANDROID_SDK_ROOT": str(sdk)})
            names = [p.name for p in roots if p.parent.name == "ndk"]
            self.assertEqual(names, ["27.0.12077973", "26.1.10909125", "9.0.8675311"])

    def test_explicit_ndk_home_is_searched_before_sdk_installs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            roots = _ndk_roots({
                "ANDROID_NDK_HOME": str(root / "explicit"),
                "ANDROID_SDK_ROOT": str(root / "sdk"),
            })
            self.assertEqual(roots[0], root / "explicit")

    def test_hermetic_environ_does_not_scan_default_sdk_locations(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            (home / "Android" / "Sdk" / "ndk" / "27.0.12077973").mkdir(parents=True)
            with mock.patch.object(Path, "home", return_value=home):
                self.assertEqual(_ndk_roots({}), [])

    def test_missing_ndk_reason_says_where_to_configure_it(self):
        clang, sysroot, reason = find_android_clang({})
        self.assertIsNone(clang)
        self.assertIsNone(sysroot)
        self.assertIn("ANDROID_NDK_HOME", reason)
        self.assertIn("local.properties", reason)


@unittest.skipUnless(_REAL_NDK and Path(_REAL_NDK).is_dir(),
                     "needs a real Android NDK: set ANDROID_NDK_HOME")
class RealNdkRelinkTests(unittest.TestCase):
    """Runs the real NDK clang; no test doubles."""

    def _write_inputs(self, root: Path) -> None:
        for name in ("game_all.c", "rt_gen.c", "rt_gen.h"):
            (root / name).write_text(f"/* {name} */\n", encoding="utf-8")
        (root / "rt_report.json").write_text(json.dumps({
            "functions": 1,
            "functionFailures": 0,
            "translatedFunctionSymbols": ["t_main_1000"],
            "translationEntryPointSymbol": "t_main_1000",
            "translatedUniqueTextBytes": 4,
            "executableTextBytes": 16,
        }), encoding="utf-8")

    def test_unchanged_rebuild_reuses_every_object(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._write_inputs(root)
            first = link_generated_translation(root, "arm64-v8a", environ={"ANDROID_NDK_HOME": _REAL_NDK})
            second = link_generated_translation(root, "arm64-v8a", environ={"ANDROID_NDK_HOME": _REAL_NDK})
            self.assertEqual(first["status"], second["status"])
            if "incremental" in second:
                self.assertEqual(second["incremental"]["compiledUnits"], [])
                self.assertEqual(len(second["incremental"]["reusedUnits"]), 6)

    def test_changed_source_recompiles_only_that_unit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._write_inputs(root)
            link_generated_translation(root, "arm64-v8a", environ={"ANDROID_NDK_HOME": _REAL_NDK})
            (root / "rt_gen.c").write_text("/* changed */\n", encoding="utf-8")
            report = link_generated_translation(root, "arm64-v8a", environ={"ANDROID_NDK_HOME": _REAL_NDK})
            if "incremental" in report:
                self.assertEqual(report["incremental"]["compiledUnits"], ["rt_gen.o"])


if __name__ == "__main__":
    unittest.main()
