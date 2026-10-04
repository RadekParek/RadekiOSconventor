from __future__ import annotations
import datetime
import hashlib
import json
import tempfile
import time
from pathlib import Path
from .archive import extract_ipa, discover_app, read_plist, metadata
from .analysis import analyze, dependency_graph, prove_leaf, capabilities
from .api_translation import generate as generate_api_replacements
from .c_backend import emit as emit_c
from .elf import inspect as inspect_elf
from .elf_writer import build_shared_object
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
                "status": "NOT_ATTEMPTED",
                "attempted": False,
                "generatedApiReplacements": 0,
                "linkedApiReplacements": 0,
                "codeGenerated": False,
                "completeGameConversion": False,
                "message": "The bounded time-API replacement subset has not been checked against entry-reachable imports.",
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
                # static call attribution. Nothing here executes the imported code.
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
                api_translation = generate_api_replacements(reconstruction, self.output)
                api_translation["reconstructedApiUseCount"] = self.report["reconstructionSummary"].get("usedApis", 0)
                self.report["apiTranslation"] = api_translation
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
                            "API imports referenced by reconstructed function calls: "
                            f'{summary.get("usedApis", 0)} used, {summary.get("nativeApis", 0)} same-name native '
                            f'candidates, {summary.get("blockedApis", 0)} without an identified Android target '
                            f'(see reconstruction.md)'
                        )
                    self.transition("BLOCKED", str(exc))
                    return self.report
                # Emit a real, self-contained Android function artifact from the
                # same statically proven leaf. The shared object has no imports,
                # relocations, game entry point, assets, or lifecycle integration.
                llvm_text = emit_llvm_ir(program)
                llvm_path = self.output / "leaf-experiment.ll"
                llvm_path.write_text(llvm_text, encoding="utf-8")
                llvm_verification = verify_llvm_ir(llvm_text)
                self.report["llvmLift"] = {
                    "status": "ENTRY_SUBSET_ONLY",
                    "irPath": llvm_path.name,
                    "functionCount": 1,
                    "instructionCount": len(program.blocks[0].instructions),
                    "sourceArchitecture": selected["architecture"],
                    "targetAbi": program.target_abi,
                    "verifiedByLlvmAs": llvm_verification["status"] == "VERIFIED",
                    "llvmAsVerification": llvm_verification,
                    "completeGameConversion": False,
                    "message": (
                        "Supplementary LLVM IR for one proven closed-integer leaf; not recovered game source "
                        "or a complete game-code port."
                    ),
                }

                c_source = emit_c(program)
                c_path = self.output / "translated-entry.c"
                c_path.write_text(c_source, encoding="utf-8")
                machine_code_path = self.output / "translated-entry.bin"
                machine_code_path.write_bytes(program.machine_code)
                shared_object = build_shared_object(program.machine_code, program.output_architecture)
                shared_object_path = self.output / "libtranslated-entry.so"
                shared_object_path.write_bytes(shared_object)
                elf_report = inspect_elf(shared_object)
                exported = elf_report.get("exports", {}).get("radek_translated_entry")
                expected_code_hash = hashlib.sha256(program.machine_code).hexdigest()
                if (
                    elf_report["architecture"] != program.target_abi
                    or exported is None
                    or exported.get("size") != len(program.machine_code)
                    or exported.get("sha256") != expected_code_hash
                    or elf_report.get("undefinedSymbols")
                ):
                    raise RuntimeError("generated Android entry library failed static ELF/export verification")
                self.report["nativeCodeArtifact"] = {
                    "status": "STANDALONE_ENTRY_FUNCTION_ONLY",
                    "sharedLibraryPath": shared_object_path.name,
                    "machineCodePath": machine_code_path.name,
                    "portableCPath": c_path.name,
                    "exportedSymbol": "radek_translated_entry",
                    "targetAbi": program.target_abi,
                    "machineCodeBytes": len(program.machine_code),
                    "sourceBytes": program.source_size,
                    "machineCodeSha256": expected_code_hash,
                    "sharedLibrarySha256": hashlib.sha256(shared_object).hexdigest(),
                    "elfValidation": elf_report,
                    "undefinedSymbols": elf_report.get("undefinedSymbols", []),
                    "relocations": 0,
                    "linkedIntoGame": False,
                    "apkProduced": False,
                    "androidDeviceLoadTest": "NOT_RUN",
                    "completeGameConversion": False,
                    "message": (
                        "A loadable ARM Android shared object exports one translated integer entry function. "
                        "It is not linked into a game, does not contain API replacements, and is not a game APK."
                    ),
                }
                self.log(
                    "NATIVE_ENTRY_TRANSLATION",
                    "Wrote and statically validated one standalone Android ELF function; no game APK was emitted",
                )
                # This function has no calls, memory accesses, or address references,
                # so its standalone library needs no dependency edges. That does not
                # implement any linked framework or other game code.
                for edge in self.report.get("dependencies", {}).get("edges", []):
                    edge["classification"] = "not-required-by-standalone-entry"
                    edge["reason"] = (
                        "The isolated translated entry function has no calls, memory accesses, or address "
                        "references. This does not implement the linked framework or the rest of the game."
                    )
                self.report["selectedArchitecture"] = selected["architecture"]
                self.report["targetAbi"] = program.target_abi
                total_text_bytes = sum(
                    int(section.get("size", 0))
                    for segment in selected.get("segments", [])
                    if int(segment.get("initialProtection", 0)) & 4
                    for section in segment.get("sections", [])
                    if section.get("name") == "__text"
                )
                if total_text_bytes <= 0 or program.source_size <= 0 or program.source_size > total_text_bytes:
                    raise RuntimeError("cannot compute verified __text byte coverage for translated entry")
                translated_percent = round(100.0 * program.source_size / total_text_bytes, 6)

                leaf_assessment = program.report()
                leaf_assessment["loweredBytesInMemory"] = leaf_assessment.pop("outputBytes")
                leaf_assessment["loweredCodeSha256InMemory"] = leaf_assessment.pop("machineCodeSha256")
                self.report["leafTranslationAssessment"] = {
                    **leaf_assessment,
                    "status": "TRANSLATED_ENTRY_ARTIFACT",
                    "machineCodeGeneratedInMemory": True,
                    "completeGameConversion": False,
                    "nativeCodeWritten": True,
                    "nativeCodeLinkedIntoGame": False,
                    "nativeCodeArtifact": shared_object_path.name,
                    "portableCArtifact": c_path.name,
                    "apkProduced": False,
                    "message": (
                        "One statically proven closed-integer entry function was translated and written as a "
                        "standalone Android ELF shared object. It is not linked into the game APK; the rest "
                        "of the game, lifecycle, resources, and reachable APIs remain untranslated."
                    ),
                }
                self.report["portProgress"] = {
                    "percent": translated_percent,
                    "status": "PARTIAL_ENTRY_CODE_TRANSLATED",
                    "metric": "translated source bytes / executable __text bytes in the selected Mach-O slice",
                    "translatedFunctions": 1,
                    "totalTextBytes": total_text_bytes,
                    "translatedTextBytes": program.source_size,
                    "completeGameConversion": False,
                    "basis": (
                        f"{program.source_size} source instruction bytes were translated into a standalone "
                        f"{program.target_abi} function ({translated_percent:.6f}% of this slice's executable "
                        "__text bytes). This is not gameplay, whole-app function coverage, API/link integration, "
                        "or APK progress."
                    ),
                }
                self.report["conversionProgress"] = {
                    "percent": 0,
                    "stage": "NOT_BUILT",
                    "status": "NOT_BUILT",
                    "message": (
                        "No APK was produced. The isolated translated function and any generated API shims "
                        "were not linked to a game or Android launcher."
                    ),
                }
                blocker = (
                    "No complete iOS-to-Android game converter is implemented: the restricted leaf "
                    "assessment does not translate the game's full reachable code, APIs, lifecycle, or assets."
                )
                self.report.setdefault("blockers", []).append(blocker)
                if analyze_only:
                    self.transition(
                        "PARTIAL",
                        "Analysis emitted one verified entry-function artifact; the rest of the game and any APK remain unconverted.",
                    )
                else:
                    self.transition(
                        "BLOCKED",
                        "Complete game conversion is unsupported; retained the isolated code artifact but refused an incomplete or placeholder game APK.",
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
