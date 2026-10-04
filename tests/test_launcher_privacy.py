"""Generated launcher UI must not expose the static-analysis report."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLACEHOLDER_ACTIVITY = ROOT / "placeholder-template/src/main/java/dev/radek/generated/GeneratedPlaceholderActivity.java"
PLACEHOLDER_BUILDER = ROOT / "app/src/main/java/dev/radek/conventor/PlaceholderApkBuilder.kt"


class PlaceholderLauncherPrivacyTests(unittest.TestCase):
    def test_analysis_stays_in_metadata_and_is_not_rendered_on_launch(self):
        activity = PLACEHOLDER_ACTIVITY.read_text(encoding="utf-8")
        builder = PLACEHOLDER_BUILDER.read_text(encoding="utf-8")

        self.assertNotIn("analysisStats", activity)
        self.assertNotIn("analysisSummary", activity)
        self.assertNotIn("Static analysis", activity)
        self.assertIn('.put("analysisSummary", analysisSummary)', builder)
        self.assertIn('.put("analysisOnly", analysisInfo)', builder)


if __name__ == "__main__":
    unittest.main()
