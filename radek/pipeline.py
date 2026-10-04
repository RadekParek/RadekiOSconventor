from __future__ import annotations
import datetime
import json
import tempfile
import time
from pathlib import Path
from .archive import extract_ipa, discover_app, read_plist, metadata
from .analysis import analyze, dependency_graph, prove_leaf, capabilities
from .icons import extract as extract_icon, launcher as launcher_icon
from .ir import Unsupported
from .llvm_ir import emit as emit_llvm_ir, verify as verify_llvm_ir
from .recon import reconstruct
from .recon.report import blockers as recon_blockers, markdown as recon_markdown, summary as recon_summary

STATES = {
    "IMPORTED",
    "ANALYZING",
    "CONVERTING",
    "PACKAGING",
    "VALIDATING",
    "READY",
    "PARTIAL",
    "BLOCKED",
    "FAILED",
}
TRANSITIONS = {
    None: {"IMPORTED", "FAILED"},
    "IMPORTED": {"ANALYZING", "FAILED"},
    "ANALYZING": {"CONVERTING", "PARTIAL", "BLOCKED", "FAILED"},
    "CONVERTING": {"PACKAGING", "BLOCKED", "FAILED"},
    "PACKAGING": {"VALIDATING", "FAILED"},
    "VALIDATING": {"READY", "FAILED"},
}


class Pipeline:
    def __init__(self, output: Path):
        self.output = output.resolve()
        self.output.mkdir(parents=True, exist_ok=False, mode=0o700)
        self.report = {
            "schemaVersion": 1,
            "state": None,
            "events": [],
            "capabilities": capabilities(),
            "apiTranslation": {
                "status": "NOT_IMPLEMENTED",
                "attempted": False,
                "generatedApiReplacements": 0,
                "codeGenerated": False,
                "message": "No verified Android API replacement backend is implemented.",
            },
            "llvmLift": {
                "status": "NOT_ATTEMPTED",
                "irPath": None,
                "functionCount": 0,
                "verifiedByLlvmAs": False,
                "completeGameConversion": False,
                "message": (
                    "No LLVM IR has been emitted; the optional leaf backend is limited to a closed integer subset."
                ),
            },
            "portProgress": {
                "percent": 0,
                "status": "NO_COMPLETE_GAME_CODE_EMITTED",
                "basis": "No complete runnable game code has been written.",
            },
            "conversionProgress": {
                "percent": 0,
                "stage": "NOT_BUILT",
                "status": "NOT_BUILT",
                "message": "No complete iOS-to-Android game conversion backend is implemented.",
            },
        }
        self._last_save = None

    def save(self, force: bool = False):
        # Progress can arrive faster than the report needs to be rewritten; every
        # event is already durable in conversion.jsonl.
        now = time.monotonic()
        if not force and self._last_save is not None and now - self._last_save < 0.2:
            return
        tmp = self.output / "report.json.tmp"
        tmp.write_text(json.dumps(self.report, indent=2, ensure_ascii=True))
        tmp.replace(self.output / "report.json")
        self._last_save = now

    def log(self, stage: str, message: str):
        event = {
            "time": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "stage": stage,
            "message": message,
        }
        self.report["events"].append(event)
        with (self.output / "conversion.jsonl").open("a") as f:
            f.write(json.dumps(event) + "\n")
        self.save()

    def transition(self, state: str, message: str):
        if state not in TRANSITIONS.get(self.report["state"], set()):
            raise RuntimeError(f'invalid transition {self.report["state"]} -> {state}')
        self.report["state"] = state
        self.log(state, message)
        self.save(force=True)

    def run(
        self,
        ipa: Path,
        authorized: bool,
        analyze_only=False,
        target_abi: str = "auto",
    ):
        try:
            if not authorized:
                raise ValueError(
                    "authorization confirmation is required; protected binaries are never decrypted"
                )
            ipa = ipa.resolve(strict=True)
            if ipa.suffix.lower() != ".ipa":
                raise ValueError("input must have .ipa extension")
            self.report["authorizationConfirmed"] = True
            self.report["source"] = {"originalName": ipa.name}
            with tempfile.TemporaryDirectory(prefix="job-", dir=self.output) as temporary:
                work = Path(temporary)
                self.transition(
                    "IMPORTED", f"Accepted IPA ({ipa.stat().st_size} bytes); starting bounded extraction"
                )
                extract_ipa(ipa, work / "extracted")
                self.transition("ANALYZING", "Extraction complete; reading plist and executable")
                app = discover_app(work / "extracted")
                info = read_plist(app / "Info.plist")
                self.report["application"] = metadata(info, ipa)
                icon_result = extract_icon(app, info, log=self.log)
                icon_report = icon_result.report()
                icon = launcher_icon(icon_result)
                if icon:
                    (self.output / "icon.png").write_bytes(icon)
                    icon_report["path"] = "icon.png"
                elif icon_result.image:
                    (self.output / "icon.png").write_bytes(icon_result.image)
                    icon_report["path"] = "icon.png"
                self.report["icon"] = icon_report
                executable = app / info["CFBundleExecutable"]
                mach = analyze(executable)
                graph = dependency_graph(app, executable, mach)
                self.report["machO"] = mach
                self.report["dependencies"] = graph
                self.log(
                    "ANALYZING",
                    f'Analyzed {len(mach["slices"])} architecture slice(s), {len(graph["nodes"])} Mach-O image(s), {len(graph["edges"])} dependency edge(s)',
                )
                # Offline reconstruction: disassembly, CFG, IR, ObjC/Swift metadata and
                # reachable-API attribution. Nothing here executes the imported code.
                reconstruction = reconstruct(
                    app, {node["path"]: node["analysis"] for node in graph["nodes"]}, log=self.log
                )
                # The full reconstruction can be megabytes; it lives in its own file
                # so every progress event does not rewrite it into report.json.
                self.report["reconstruction"] = {
                    "schemaVersion": reconstruction["schemaVersion"],
                    "status": reconstruction["status"],
                    "imageCount": reconstruction["imageCount"],
                    "summary": recon_summary(reconstruction),
                    "jsonPath": "reconstruction.json",
                    "markdownPath": "reconstruction.md",
                }
                self.report["reconstructionSummary"] = recon_summary(reconstruction)
                self.report["capabilities"] = capabilities(reconstruction)
                (self.output / "reconstruction.json").write_text(
                    json.dumps(reconstruction, indent=2, ensure_ascii=True)
                )
                (self.output / "reconstruction.md").write_text(
                    recon_markdown(reconstruction, self.report["application"])
                )
                self.save()
                self.log(
                    "ANALYZING",
                    f'Reconstructed {reconstruction["imageCount"]} image(s): '
                    + ", ".join(
                        f'{key}={value}'
                        for key, value in self.report["reconstructionSummary"].items()
                        if key in ("functionCount", "objectiveCClasses", "swiftTypes", "usedApis")
                    ),
                )
                self.report["apiTranslation"] = {
                    "status": "NOT_IMPLEMENTED",
                    "attempted": False,
                    "generatedApiReplacements": 0,
                    "untranslatedReachableApiCount": self.report["reconstructionSummary"].get("usedApis", 0),
                    "codeGenerated": False,
                    "message": "Android API symbol matches and semantic targets are analysis candidates only; no API replacement or implementation was generated.",
                }
                try:
                    selected, program = prove_leaf(executable, mach, graph, reconstruction, target_abi)
                except Unsupported as exc:
                    self.report["llvmLift"] = {
                        "status": "BLOCKED",
                        "irPath": None,
                        "functionCount": 0,
                        "verifiedByLlvmAs": False,
                        "completeGameConversion": False,
                        "message": (
                            "The entry routine is outside the closed integer LLVM-lift subset: " + str(exc)
                        ),
                    }
                    self.report["blockers"] = [line for line in str(exc).split("; ") if line]
                    summary = self.report.get("reconstructionSummary") or {}
                    if summary:
                        self.report["blockers"].append(
                            "Reachable APIs in reconstructed code: "
                            f'{summary.get("usedApis", 0)} used, {summary.get("nativeApis", 0)} same-name native '
                            f'candidates, {summary.get("blockedApis", 0)} without an identified Android target '
                            f'(see reconstruction.md)'
                        )
                    self.transition("BLOCKED", str(exc))
                    return self.report
                # The LLVM artifact is intentionally limited to this independently
                # proven closed-integer entry leaf. It is not reconstructed game
                # source and is never fed to an APK packager.
                llvm_text = emit_llvm_ir(program)
                llvm_path = self.output / "leaf-experiment.ll"
                llvm_path.write_text(llvm_text, encoding="utf-8")
                llvm_verification = verify_llvm_ir(llvm_text)
                self.report["llvmLift"] = {
                    "status": "EXPERIMENTAL_ENTRY_ONLY",
                    "irPath": llvm_path.name,
                    "functionCount": 1,
                    "instructionCount": len(program.blocks[0].instructions),
                    "sourceArchitecture": selected["architecture"],
                    "targetAbi": program.target_abi,
                    "verifiedByLlvmAs": llvm_verification["status"] == "VERIFIED",
                    "llvmAsVerification": llvm_verification,
                    "completeGameConversion": False,
                    "message": (
                        "A single closed-integer leaf was lifted to textual LLVM IR; this is not a game-code port."
                    ),
                }
                self.log(
                    "LLVM_LIFT", "Wrote one experimental closed-integer LLVM IR leaf; no APK code was emitted"
                )
                # A proven integer leaf is only a narrow translation experiment; it
                # is not a complete iOS game. Never wrap it in a launcher APK or mark
                # it READY. Keep the assessment separate from emitted runnable code.
                for edge in self.report.get("dependencies", {}).get("edges", []):
                    edge["classification"] = "not-required-by-experimental-leaf"
                    edge["reason"] = (
                        "The isolated test leaf has no calls, memory accesses, or address "
                        "references. This does not implement the linked framework or the game."
                    )
                self.report["selectedArchitecture"] = selected["architecture"]
                self.report["targetAbi"] = program.target_abi
                leaf_assessment = program.report()
                leaf_assessment["loweredBytesInMemory"] = leaf_assessment.pop("outputBytes")
                leaf_assessment["loweredCodeSha256InMemory"] = leaf_assessment.pop("machineCodeSha256")
                self.report["leafTranslationAssessment"] = {
                    **leaf_assessment,
                    "status": "EXPERIMENTAL_SUBSET_ONLY",
                    "machineCodeGeneratedInMemory": True,
                    "completeGameConversion": False,
                    "nativeCodeWritten": False,
                    "apkProduced": False,
                    "message": (
                        "The restricted closed integer entry can be lowered in memory, but "
                        "the rest of the game, Android lifecycle, resources and APIs are not translated."
                    ),
                }
                self.report["portProgress"] = {
                    "percent": 0,
                    "status": "NO_COMPLETE_GAME_CODE_EMITTED",
                    "basis": "The leaf assessment is not a complete game port; no runnable APK code was written.",
                }
                self.report["conversionProgress"] = {
                    "percent": 0,
                    "stage": "NOT_BUILT",
                    "status": "NOT_BUILT",
                    "message": "No APK was produced because complete game translation and API replacement are not implemented.",
                }
                blocker = (
                    "No complete iOS-to-Android game converter is implemented: the restricted leaf "
                    "assessment does not translate the game's full reachable code, APIs, lifecycle, or assets."
                )
                self.report.setdefault("blockers", []).append(blocker)
                if analyze_only:
                    self.transition(
                        "PARTIAL",
                        "Analysis and restricted leaf eligibility assessment completed; no code artifact or APK was emitted.",
                    )
                else:
                    self.transition(
                        "BLOCKED",
                        "Complete game conversion is unsupported; refusing to emit an incomplete or placeholder APK.",
                    )
                return self.report
        except Exception as exc:
            self.report["error"] = {"type": type(exc).__name__, "message": str(exc)}
            if self.report["state"] not in ("READY", "PARTIAL", "BLOCKED", "FAILED"):
                self.transition("FAILED", str(exc))
            else:
                self.save(force=True)
        self.save(force=True)
        return self.report
