"""End-to-end conversion of the committed synthetic sample IPA.

The sample is deliberately inside the proven subset so the whole honest path
runs: analysis, leaf translation, compatibility-registry generation and (when
an Android toolchain is present) the labelled experimental shell. It must
never reach READY and never claim a complete-game conversion.
"""

import importlib.util
import tempfile
import unittest
from pathlib import Path

from radek.pipeline import Pipeline
from tests.fixtures import ipa, macho

_TOOL = Path(__file__).resolve().parent.parent / "tools" / "make_sample_ipa.py"
_spec = importlib.util.spec_from_file_location("make_sample_ipa", _TOOL)
_sample = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_sample)
CODE, IMPORTS, SAMPLE_PATH = _sample.CODE, _sample.IMPORTS, _sample.OUTPUT


class SampleIpaTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SAMPLE_PATH.is_file(), "committed sample IPA is missing")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_committed_sample_matches_its_generator(self):
        regenerated = ipa(Path(self.tmp.name) / "regenerated.ipa", macho(CODE, imports=IMPORTS))
        self.assertEqual(regenerated.read_bytes(), SAMPLE_PATH.read_bytes())

    def test_sample_analysis_is_partial_with_honest_registry(self):
        report = Pipeline(Path(self.tmp.name) / "analysis").run(
            SAMPLE_PATH, True, analyze_only=True
        )
        self.assertEqual(report["state"], "PARTIAL")
        self.assertGreater(report["portProgress"]["percent"], 0)
        self.assertEqual(report["portProgress"]["translatedTextBytes"], len(CODE))
        self.assertFalse(report["portProgress"]["completeGameConversion"])
        registry = report["compatRegistry"]
        self.assertEqual(registry["status"], "REGISTRY_SOURCE_GENERATED")
        self.assertEqual(registry["verifiedImplementations"], 1)
        self.assertEqual(registry["stubbedHandlers"], 2)
        self.assertEqual(registry["unresolvedImports"], 0)
        self.assertEqual(registry["handlerResolutionCoveragePercent"], 100.0)
        self.assertFalse(registry["completeGameConversion"])

    def test_sample_convert_stays_blocked_but_keeps_artifacts(self):
        output = Path(self.tmp.name) / "conversion"
        report = Pipeline(output).run(SAMPLE_PATH, True, analyze_only=False)
        self.assertEqual(report["state"], "BLOCKED")
        self.assertEqual(report["conversionProgress"]["status"], "NOT_BUILT")
        self.assertEqual(report["conversionProgress"]["percent"], 0)
        # Isolated translated artifacts were really written and validated.
        self.assertTrue((output / "libtranslated-entry.so").is_file())
        self.assertTrue((output / "translated-entry.c").is_file())
        self.assertTrue((output / "ioscompat" / "libioscompat.cpp").is_file())
        # The experimental shell is either built and labelled, skipped for
        # lack of toolchain, or honestly failed — never silently claimed.
        self.assertIn(
            report["experimentalShell"]["status"],
            {"BUILT_NOT_A_GAME", "SKIPPED_NO_ANDROID_TOOLCHAIN", "FAILED_TO_BUILD"},
        )
        self.assertFalse(report["experimentalShell"]["completeGameConversion"])


if __name__ == "__main__":
    unittest.main()
