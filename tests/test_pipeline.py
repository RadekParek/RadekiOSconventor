import json
import struct
import tempfile
import unittest
from pathlib import Path
from radek.pipeline import Pipeline
from .fixtures import ipa, macho, fat


class PipelineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def run_fixture(self, exe=None, **kwargs):
        source = ipa(self.root / "input.ipa", exe)
        return Pipeline(self.root / "job").run(source, True, **kwargs)

    def test_synthetic_arm64_analysis_is_partial_not_ready(self):
        report = self.run_fixture(analyze_only=True)
        self.assertEqual(report["state"], "PARTIAL")
        self.assertEqual(report["leafTranslationAssessment"]["backend"], "preserved-arm64")
        self.assertEqual(report["llvmLift"]["status"], "EXPERIMENTAL_ENTRY_ONLY")
        self.assertEqual(report["llvmLift"]["completeGameConversion"], False)
        self.assertTrue((self.root / "job/leaf-experiment.ll").is_file())
        self.assertEqual(report["icon"]["status"], "SUPPORTED")
        self.assertFalse(list((self.root / "job").glob("job-*")))
        self.assertFalse((self.root / "job/input.apk").exists())
        self.assertEqual(json.loads((self.root / "job/report.json").read_text())["state"], "PARTIAL")

    def test_unused_framework_dependency_needs_no_stub(self):
        result = self.run_fixture(
            macho(
                imports=["_UIApplicationMain"],
                dependencies=["/System/Library/Frameworks/UIKit.framework/UIKit"],
            ),
            analyze_only=True,
        )
        self.assertEqual(result["state"], "PARTIAL")
        self.assertEqual(result["dependencies"]["edges"][0]["classification"], "not-required-by-experimental-leaf")
        self.assertIn("does not implement the linked framework or the game", result["dependencies"]["edges"][0]["reason"])
        self.assertEqual(result["leafTranslationAssessment"]["backend"], "preserved-arm64")

    def test_reachable_framework_call_remains_blocked(self):
        result = self.run_fixture(
            macho(
                code=struct.pack("<II", 0x94000000, 0xD65F03C0),
                imports=["_UIApplicationMain"],
                dependencies=["/System/Library/Frameworks/UIKit.framework/UIKit"],
            ),
            analyze_only=True,
        )
        self.assertEqual(result["state"], "BLOCKED")

    def test_encryption_is_never_bypassed(self):
        result = self.run_fixture(macho(encrypted=True), analyze_only=True)
        self.assertEqual(result["state"], "BLOCKED")
        self.assertIn("prohibited", result["blockers"][0])

    def test_arm64e_always_blocked(self):
        self.assertEqual(self.run_fixture(macho(subtype=2), analyze_only=True)["state"], "BLOCKED")

    def test_unsafe_instruction_blocked(self):
        result = self.run_fixture(macho(code=struct.pack("<II", 0xD4000001, 0xD65F03C0)), analyze_only=True)
        self.assertEqual(result["state"], "BLOCKED")
        self.assertIn("not in the proven subset", result["blockers"][0])

    def test_relocation_blocked(self):
        self.assertEqual(self.run_fixture(macho(reloc=True), analyze_only=True)["state"], "BLOCKED")

    def test_reconstruction_report_is_written(self):
        result = self.run_fixture(
            macho(
                imports=["_UIApplicationMain"],
                dependencies=["/System/Library/Frameworks/UIKit.framework/UIKit"],
            ),
            analyze_only=True,
        )
        job = self.root / "job"
        self.assertTrue((job / "reconstruction.json").is_file())
        self.assertTrue((job / "reconstruction.md").is_file())
        self.assertEqual(result["reconstruction"]["status"], "ok")
        summary = result["reconstruction"]["summary"]
        self.assertEqual(summary["architectures"], ["arm64"])
        self.assertGreaterEqual(summary["functionCount"], 1)
        self.assertGreater(summary["coverage"], 0)
        markdown_text = (job / "reconstruction.md").read_text()
        self.assertIn("IPA reconstruction report", markdown_text)
        self.assertIn("Reconstructed coverage", markdown_text)

    def test_linked_but_unused_framework_is_not_reported_blocked(self):
        result = self.run_fixture(
            macho(
                imports=["_UIApplicationMain"],
                dependencies=["/System/Library/Frameworks/UIKit.framework/UIKit"],
            ),
            analyze_only=True,
        )
        states = {c["component"]: c["status"] for c in result["capabilities"]}
        # The import is linked but nothing in the reconstructed code reaches it.
        self.assertEqual(states["UIKit/CoreGraphics"], "SUPPORTED")
        self.assertEqual(states["Metal"], "SUPPORTED")
        self.assertIn("no reachable API use", " ".join(c["detail"] for c in result["capabilities"]))
        self.assertIn("No complete iOS-to-Android game converter", " ".join(result["blockers"]))
        self.assertEqual(result["dependencies"]["edges"][0]["classification"], "not-required-by-experimental-leaf")

    def test_icon_is_recovered_from_assets_car(self):
        from .test_icons import catalog_bytes

        source = ipa(
            self.root / "input.ipa",
            icon=False,
            extra={"Payload/Fixture.app/Assets.car": catalog_bytes()},
        )
        result = Pipeline(self.root / "job").run(source, True, analyze_only=True)
        self.assertEqual(result["icon"]["status"], "SUPPORTED")
        self.assertEqual(result["icon"]["kind"], "assets.car")
        self.assertEqual(result["icon"]["decoder"], "assetcatalog+pngcodec")
        self.assertTrue((self.root / "job/icon.png").is_file())

    def test_authorization_required(self):
        result = Pipeline(self.root / "job").run(ipa(self.root / "input.ipa"), False)
        self.assertEqual(result["state"], "FAILED")
        self.assertIn("authorization", result["error"]["message"])

    def test_convert_refuses_restricted_leaf_apk(self):
        result = self.run_fixture()
        self.assertEqual(result["state"], "BLOCKED")
        self.assertEqual(result["conversionProgress"]["status"], "NOT_BUILT")
        self.assertEqual(result["llvmLift"]["status"], "EXPERIMENTAL_ENTRY_ONLY")
        self.assertEqual(result["portProgress"]["percent"], 0)
        self.assertFalse(result["leafTranslationAssessment"]["apkProduced"])
        self.assertEqual(result["apiTranslation"]["generatedApiReplacements"], 0)
        self.assertFalse(result["apiTranslation"]["codeGenerated"])
        self.assertFalse(list((self.root / "job").glob("*.apk")))

    def test_embedded_framework_graph(self):
        main = macho(dependencies=["@executable_path/Frameworks/Embedded.framework/Embedded"])
        source = ipa(
            self.root / "input.ipa",
            main,
            extra={"Payload/Fixture.app/Frameworks/Embedded.framework/Embedded": macho()},
        )
        result = Pipeline(self.root / "job").run(source, True, True)
        self.assertEqual(result["state"], "BLOCKED")
        self.assertEqual(
            result["dependencies"]["edges"][0]["resolvedBundlePath"], "Frameworks/Embedded.framework/Embedded"
        )

    def test_damaged_bind_stream_is_analyzed_but_remains_blocked(self):
        bind = b"\x40_symbol\x00\x70" + b"\xff" * 9 + b"\x01\x80\x01\x00"
        command = struct.pack("<12I", 0x80000022, 48, 0, 0, 0x2000, len(bind), 0, 0, 0, 0, 0, 0)
        result = self.run_fixture(macho(extras=[command], blobs={0x2000: bind}), analyze_only=True)
        self.assertEqual(result["state"], "BLOCKED")
        slice_data = result["machO"]["slices"][0]
        self.assertFalse(slice_data["bindDecodingComplete"])
        self.assertTrue(any("bind table is incomplete" in item for item in result["blockers"]))

    def test_unknown_loader_command_blocked(self):
        result = self.run_fixture(macho(extras=[struct.pack("<II", 0x777, 8)]), analyze_only=True)
        self.assertEqual(result["state"], "BLOCKED")

    def test_fat_selects_safe_arm64(self):
        result = self.run_fixture(fat([macho(cpu=12, subtype=9), macho()]), analyze_only=True)
        self.assertEqual(result["selectedArchitecture"], "arm64")
        self.assertEqual(result["targetAbi"], "arm64-v8a")

    def test_armv6_and_thumb_plans_target_32bit_armv7(self):
        arm_mode = self.run_fixture(macho(cpu=12, subtype=6), analyze_only=True)
        self.assertEqual(arm_mode["state"], "PARTIAL")
        self.assertEqual(arm_mode["selectedArchitecture"], "armv6")
        self.assertEqual(arm_mode["leafTranslationAssessment"]["backend"], "offline-armv6-to-armv7")
        self.assertEqual(arm_mode["leafTranslationAssessment"]["targetAbi"], "armeabi-v7a")
        self.assertIn(
            'target triple = "armv7-unknown-linux-androideabi"',
            (self.root / "job/leaf-experiment.ll").read_text(),
        )

        thumb_source = ipa(
            self.root / "thumb.ipa",
            macho(struct.pack("<HH", 0x202A, 0x4770), cpu=12, subtype=6, thumb=True),
        )
        thumb_mode = Pipeline(self.root / "job-thumb").run(thumb_source, True, analyze_only=True)
        self.assertEqual(thumb_mode["state"], "PARTIAL")
        self.assertEqual(thumb_mode["selectedArchitecture"], "armv6")
        self.assertEqual(thumb_mode["leafTranslationAssessment"]["loweredBytesInMemory"], 8)
        self.assertEqual(thumb_mode["leafTranslationAssessment"]["targetAbi"], "armeabi-v7a")

    def test_thumb_plan_targets_32bit_armv7(self):
        result = self.run_fixture(
            macho(struct.pack("<HH", 0x202A, 0x4770), cpu=12, subtype=9, thumb=True), analyze_only=True
        )
        self.assertEqual(result["leafTranslationAssessment"]["backend"], "offline-armv7-to-armv7")
        self.assertEqual(result["leafTranslationAssessment"]["targetAbi"], "armeabi-v7a")
        self.assertEqual(result["leafTranslationAssessment"]["loweredBytesInMemory"], 8)

    def test_explicit_target_abi_requires_a_matching_source_slice(self):
        arm32 = ipa(self.root / "arm32.ipa", macho(cpu=12, subtype=6))
        blocked = Pipeline(self.root / "arm64-target").run(
            arm32, True, analyze_only=True, target_abi="arm64-v8a"
        )
        self.assertEqual(blocked["state"], "BLOCKED")
        self.assertIn("requires an ARM64 IPA slice", " ".join(blocked["blockers"]))

        dual_arch = ipa(self.root / "dual.ipa", fat([macho(cpu=12, subtype=11), macho()]))
        selected = Pipeline(self.root / "arm32-target").run(
            dual_arch, True, analyze_only=True, target_abi="armeabi-v7a"
        )
        self.assertEqual(selected["state"], "PARTIAL")
        self.assertEqual(selected["selectedArchitecture"], "armv7s")
        self.assertEqual(selected["targetAbi"], "armeabi-v7a")

    def test_invalid_state_transition(self):
        p = Pipeline(self.root / "job")
        with self.assertRaises(RuntimeError):
            p.transition("READY", "not validated")

    def test_no_graphics_audio_swift_support_claims(self):
        p = Pipeline(self.root / "job")
        caps = {c["component"]: c["status"] for c in p.report["capabilities"]}
        for name in (
            "EAGL/OpenGL ES",
            "AudioToolbox/AVFoundation/OpenAL",
            "Swift",
            "Metal",
            "Foundation/CoreFoundation",
            "UIKit/CoreGraphics",
            "iOS lifecycle/input/sensors",
        ):
            self.assertEqual(caps[name], "BLOCKED")
