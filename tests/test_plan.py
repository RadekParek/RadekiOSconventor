"""Host static-recompilation plan coverage (``radek.plan``).

The plan pass measures the game's own code bytes that the fail-closed host
lifter translates into portable C. These tests pin the honesty rules: it never
claims a linked game or device code, it degrades to ``UNAVAILABLE`` instead of
raising, and the pipeline records it without touching ``conversionProgress``.

The real-run test uses the committed authorized Angry Birds IPA, because the
game lifter targets classic 32-bit ARM Mach-O slices and that is the input the
plan exists for.
"""

import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from radek.plan import LIMITATIONS, plan_coverage
from tests import fixtures

ANGRY_BIRDS_IPA = Path(__file__).resolve().parent / "data" / "AngryBirds_v1.0_os30.ipa"


def _capstone_available() -> bool:
    try:
        import capstone
        version = tuple(map(int, capstone.__version__.split(".")[:3]))
    except Exception:
        return False
    return (5, 0, 6) <= version < (6, 0, 0)


CAPSTONE_AVAILABLE = _capstone_available()


def _angry_birds_executable() -> bytes:
    with zipfile.ZipFile(ANGRY_BIRDS_IPA) as archive:
        return archive.read("Payload/AngryBirds.app/AngryBirds")


class PlanModuleTest(unittest.TestCase):
    def test_unreadable_input_reports_unavailable_without_raising(self):
        result = plan_coverage(b"this is not a mach-o image at all")
        self.assertEqual(result["status"], "UNAVAILABLE")
        self.assertEqual(result["percent"], 0)
        self.assertIn("reason", result)
        self.assertEqual(result["limitations"], list(LIMITATIONS))

    def test_missing_path_reports_unavailable(self):
        result = plan_coverage(Path(tempfile.gettempdir()) / "definitely-missing-fixture.bin")
        self.assertEqual(result["status"], "UNAVAILABLE")
        self.assertEqual(result["percent"], 0)

    def test_64_bit_images_are_out_of_scope_for_the_game_lifter(self):
        result = plan_coverage(fixtures.macho())
        self.assertEqual(result["status"], "UNAVAILABLE")
        self.assertIn("32-bit ARM", result["reason"])


@unittest.skipUnless(
    CAPSTONE_AVAILABLE and ANGRY_BIRDS_IPA.is_file(),
    "requires capstone and the committed authorized Angry Birds IPA",
)
class PlanRealInputTest(unittest.TestCase):
    """One real blocked game: the plan must measure code and claim nothing else."""

    @classmethod
    def setUpClass(cls):
        from radek.pipeline import Pipeline

        cls.temporary = tempfile.TemporaryDirectory()
        root = Path(cls.temporary.name)
        cls.report = Pipeline(root / "job").run(ANGRY_BIRDS_IPA, True, analyze_only=True)
        cls.root = root

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_plan_measures_real_code_without_claiming_linkage(self):
        plan = self.report["staticRecompilationPlan"]
        self.assertIn(plan["status"], ("COMPUTED", "TRUNCATED"))
        self.assertGreater(plan["functionsDiscovered"], 100)
        self.assertGreater(plan["staticallyRecompiledBytes"], 0)
        self.assertGreater(plan["percent"], 0)
        self.assertLessEqual(plan["percent"], 100)
        self.assertFalse(plan["linkedIntoGame"])
        self.assertFalse(plan["completeGameConversion"])
        self.assertFalse(plan["countsAsConversionProgress"])
        self.assertFalse(plan["codeGeneratedOnDevice"])
        self.assertIn("not gameplay", plan["basis"])

    def test_plan_never_completes_a_conversion(self):
        # A measured plan is host static-recompilation coverage, not a game APK.
        self.assertEqual(self.report["state"], "BLOCKED")
        self.assertEqual(self.report["conversionProgress"]["status"], "NOT_BUILT")
        self.assertEqual(self.report["conversionProgress"]["percent"], 0)
        host_progress = self.report["hostStaticRecompilationProgress"]
        self.assertGreater(host_progress["percent"], 0)
        self.assertFalse(host_progress["androidLinkVerified"])
        self.assertFalse(host_progress["completeGameConversion"])
        android_link = self.report["androidLink"]
        if android_link["status"] == "VERIFIED_ANDROID_SHARED_LIBRARY":
            self.assertTrue(android_link["architectureVerified"])
            self.assertTrue(android_link["dependenciesVerified"])
            self.assertTrue(android_link["exportsVerified"])
            self.assertTrue(android_link["allVerificationsPassed"])
            self.assertGreater(self.report["portProgress"]["percent"], 0)
            self.assertTrue(self.report["portProgress"]["androidLinkVerified"])
        else:
            self.assertEqual(self.report["portProgress"]["percent"], 0)
            self.assertEqual(android_link["androidLinkedTextBytes"], 0)
            self.assertFalse(self.report["portProgress"]["androidLinkVerified"])
            self.assertFalse(android_link["allVerificationsPassed"])
        self.assertFalse(self.report["portProgress"]["linkedIntoGame"])
        self.assertFalse(self.report["portProgress"]["completeGameConversion"])

    def test_plan_is_durable_in_the_saved_report(self):
        saved = json.loads((self.root / "job" / "report.json").read_text())
        self.assertEqual(
            saved["staticRecompilationPlan"]["status"],
            self.report["staticRecompilationPlan"]["status"],
        )
        self.assertEqual(
            saved["hostStaticRecompilationProgress"]["recompiledTextBytes"],
            self.report["staticRecompilationPlan"]["staticallyRecompiledBytes"],
        )
        self.assertEqual(saved["portProgress"]["percent"], self.report["portProgress"]["percent"])

    def test_whole_game_bytecode_translation_is_emitted_before_packaging_blocker(self):
        translation = self.report["bytecodeTranslation"]
        self.assertEqual(translation["status"], "GENERATED_PORTABLE_C")
        self.assertGreater(translation["translatedFunctionCount"], 1000)
        self.assertEqual(translation["functionFailures"], 0)
        self.assertGreater(translation["percent"], 0)
        self.assertLessEqual(translation["percent"], 100.0)
        self.assertFalse(translation["linkedIntoGame"])
        self.assertFalse(translation["apkProduced"])
        self.assertEqual(translation["translatedFunctionCount"], translation["translatedFunctionSymbolsCount"])
        self.assertLessEqual(translation["translatedTextBytes"], translation["executableTextBytes"])
        link = self.report["androidLink"]
        if link["status"] == "VERIFIED_ANDROID_SHARED_LIBRARY":
            self.assertEqual(link["linkedTranslatedFunctionCount"], translation["translatedFunctionCount"])
            self.assertEqual(self.report["portProgress"]["percent"], link["androidLinkedTextPercent"])
            self.assertTrue(self.report["portProgress"]["androidLinkVerified"])
            self.assertFalse(link["linkedIntoGame"])
            self.assertFalse(link["apkProduced"])
        else:
            self.assertEqual(self.report["portProgress"]["percent"], 0)
            self.assertEqual(link["androidLinkedTextBytes"], 0)
        job = self.root / "job"
        for name in ("game_all.c", "rt_gen.c", "rt_gen.h", "rt_mem.bin", "rt_report.json"):
            self.assertTrue((job / "bytecode-translation" / name).is_file(), name)
        generated = json.loads((job / "bytecode-translation" / "rt_report.json").read_text())
        self.assertEqual(generated["functions"], translation["translatedFunctionCount"])
        self.assertEqual(generated["functionFailures"], 0)
        self.assertEqual(generated["objcClasses"], 2)
        self.assertEqual(generated["objcMethods"], 25)
        self.assertEqual(generated["objcProblems"], 0)
        self.assertEqual(generated["lsdaTables"], 558)
        self.assertEqual(generated["lsdaSites"], 2907)
        self.assertEqual(generated["lsdaActions"], 51)
        self.assertEqual(generated["lsdaProblems"], 0)


class PlanProvenSubsetTest(unittest.TestCase):
    """The bounded proven subset keeps its own artifact accounting."""

    def test_proven_subset_never_reports_a_host_plan(self):
        from radek.pipeline import Pipeline

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = fixtures.ipa(root / "fixture.ipa")
            report = Pipeline(root / "job").run(source, True, analyze_only=True)

        self.assertNotIn("staticRecompilationPlan", report)
        self.assertNotIn("hostPlanOnly", report["portProgress"])


if __name__ == "__main__":
    unittest.main()
