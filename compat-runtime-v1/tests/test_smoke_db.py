import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from argparse import Namespace

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "smoke_db.py"
SPEC = importlib.util.spec_from_file_location("compat_runtime_smoke_db", SCRIPT)
smoke_db = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(smoke_db)


class BuildScopeTests(unittest.TestCase):
    def test_converter_and_runtime_modules_select_separate_native_targets(self):
        root = Path(__file__).resolve().parents[2]
        converter_gradle = (root / "app" / "build.gradle.kts").read_text(encoding="utf-8")
        runtime_gradle = (root / "compat-runtime-v1" / "build.gradle.kts").read_text(encoding="utf-8")
        cmake = (root / "native" / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("-DRADEK_BUILD_COMPAT_RUNTIME=OFF", converter_gradle)
        self.assertIn('option(RADEK_BUILD_COMPAT_RUNTIME "Build and test compat-runtime-v1" OFF)', cmake)
        self.assertIn('option(RADEK_FETCH_UNICORN "Fetch the pinned Unicorn ARM32 backend" OFF)', cmake)
        self.assertIn('"-DRADEK_BUILD_COMPAT_RUNTIME=ON"', runtime_gradle)
        self.assertIn('"-DRADEK_FETCH_UNICORN=ON"', runtime_gradle)
        self.assertIn('targets += "compat_runtime_v1"', runtime_gradle)


class SmokeDatabaseTests(unittest.TestCase):
    def test_initial_database_is_explicitly_unmeasured(self):
        data = smoke_db.load_database(smoke_db.DEFAULT_DATABASE)
        self.assertEqual(data["runtimeContract"], "compat-runtime-v1")
        self.assertEqual(data["apps"], [])
        self.assertEqual([family["rank"] for family in data["shimBacklog"]], [1, 2, 3])
        self.assertTrue(all(not family["gamesUnblockedMeasured"] for family in data["shimBacklog"]))

    def test_status_vocabulary_fails_closed(self):
        for status in ("not runnable", "crashes at _objc_msgSend", "menu", "playable"):
            with self.subTest(status=status):
                smoke_db.validate_status(status)
        for status in ("probably playable", "crashes", "menu-ish", "99%"):
            with self.subTest(status=status), self.assertRaises(ValueError):
                smoke_db.validate_status(status)

    def test_record_smoke_is_repeatable_and_tracks_unblocked_titles(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / "database.json"
            database.write_text(smoke_db.DEFAULT_DATABASE.read_text(encoding="utf-8"), encoding="utf-8")
            args = Namespace(
                database=str(database),
                report=None,
                status="menu",
                evidence="device capture run 2026-10-05",
                family_resolved=["objc-runtime-core"],
                first_missing_import=None,
                app_id="sample-game-v1",
                name="Sample Game",
                version="1.0",
                device="Android test device",
                runtime_build="host-smoke-test",
            )
            smoke_db.record_smoke(args)
            args.status = "playable"
            args.evidence = "repeat play-session capture run 2026-10-05"
            smoke_db.record_smoke(args)

            saved = smoke_db.load_database(database)
            self.assertEqual(len(saved["apps"]), 1)
            app = saved["apps"][0]
            self.assertEqual(app["latestSmoke"]["status"], "playable")
            self.assertEqual(len(app["smokeRuns"]), 2)
            family = saved["shimBacklog"][0]
            self.assertEqual(family["gamesUnblocked"], ["sample-game-v1"])
            self.assertTrue(family["gamesUnblockedMeasured"])

    def test_families_cannot_be_claimed_from_a_not_runnable_result(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / "database.json"
            database.write_text(smoke_db.DEFAULT_DATABASE.read_text(encoding="utf-8"), encoding="utf-8")
            args = Namespace(
                database=str(database), report=None, status="not runnable",
                evidence="runtime report", family_resolved=["objc-runtime-core"],
                first_missing_import="_objc_msgSend", app_id="game-v1", name="Game",
                version="unknown", device=None, runtime_build=None,
            )
            with self.assertRaisesRegex(ValueError, "only be marked game-unblocking"):
                smoke_db.record_smoke(args)

    def test_runtime_report_symbol_names_are_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = root / "database.json"
            report_path = root / "compat-runtime-v1-report.json"
            database.write_text(smoke_db.DEFAULT_DATABASE.read_text(encoding="utf-8"), encoding="utf-8")
            report_path.write_text(json.dumps({
                "runtimeContract": "compat-runtime-v1",
                "firstMissingImport": "_objc_msgSend",
                "resolvedSymbols": [{"symbol": "_CFRelease"}],
                "unresolvedSymbols": [{"symbol": "_objc_msgSend"}],
            }), encoding="utf-8")
            args = Namespace(
                database=str(database), report=str(report_path), status="not runnable",
                evidence="runtime report from device import", family_resolved=[],
                first_missing_import=None, app_id="game-v1", name="Game", version="1.2",
                device="Android test device", runtime_build="runtime-v1-test",
            )
            smoke_db.record_smoke(args)
            app = smoke_db.load_database(database)["apps"][0]
            self.assertEqual(app["latestSmoke"]["firstMissingImport"], "_objc_msgSend")
            self.assertEqual(app["latestSmoke"]["resolvedSymbols"], ["_CFRelease"])
            self.assertEqual(app["latestSmoke"]["unresolvedSymbols"], ["_objc_msgSend"])


if __name__ == "__main__":
    unittest.main()
