from __future__ import annotations
import datetime
import json
import shutil
import tempfile
import time
from pathlib import Path
from .archive import extract_ipa, discover_app, read_plist, metadata
from .analysis import analyze, dependency_graph, prove_leaf, capabilities
from .icons import extract as extract_icon, launcher as launcher_icon
from .ir import Unsupported
from .recon import reconstruct
from .recon.report import blockers as recon_blockers, markdown as recon_markdown, summary as recon_summary
from .resources import copy_resources
from .apk import ARTIFACT, Toolchain, build_apk, validate_apk

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
        self.report = {"schemaVersion": 1, "state": None, "events": [], "capabilities": capabilities()}
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

    def run(self, ipa: Path, authorized: bool, analyze_only=False, key: Path | None = None):
        try:
            if not authorized:
                raise ValueError(
                    "authorization confirmation is required; protected binaries are never decrypted"
                )
            ipa = ipa.resolve(strict=True)
            if ipa.suffix.lower() != ".ipa":
                raise ValueError("input must have .ipa extension")
            self.report["authorizationConfirmed"] = True
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
                try:
                    selected, program = prove_leaf(executable, mach, graph, reconstruction)
                except Unsupported as exc:
                    self.report["blockers"] = [line for line in str(exc).split("; ") if line]
                    summary = self.report.get("reconstructionSummary") or {}
                    if summary:
                        self.report["blockers"].append(
                            "Reachable APIs in reconstructed code: "
                            f'{summary.get("usedApis", 0)} used, {summary.get("nativeApis", 0)} natively '
                            f'implementable, {summary.get("blockedApis", 0)} without any Android mapping '
                            f'(see reconstruction.md)'
                        )
                    self.transition("BLOCKED", str(exc))
                    return self.report
                self.report["selectedArchitecture"] = selected["architecture"]
                # The accepted leaf has no call, memory, or address operations. Any
                # linked dependency in this single-image case is therefore unused by
                # the emitted code; do not synthesize symbols or ship no-op stubs.
                unused_edges = self.report.get("dependencies", {}).get("edges", [])
                for edge in unused_edges:
                    edge["classification"] = "not-required-by-proven-entry"
                    edge["reason"] = (
                        "The emitted closed integer entry has no calls, memory accesses, "
                        "or address references; no framework stub/provider was linked."
                    )
                self.report["conversion"] = program.report()
                self.report["contract"] = "closed-integer-entry-v1"
                if analyze_only:
                    self.transition(
                        "PARTIAL",
                        "Verified leaf conversion plan; packaging was not requested. No APK has been produced.",
                    )
                    return self.report
                tools = Toolchain.discover()
                self.transition(
                    "CONVERTING",
                    f"Reconstructed {program.source_size} input bytes into {len(program.machine_code)} Android ARM64 instruction bytes",
                )
                assets = work / "assets"
                self.report["resources"] = copy_resources(app, assets / "bundle", info["CFBundleExecutable"])
                self.log(
                    "CONVERTING",
                    f'Preserved {len(self.report["resources"])} resource files with relative bundle paths',
                )
                self.transition(
                    "PACKAGING",
                    "Compiling JNI ELF, Android entry point, resources and DEX; signing with development key",
                )
                pending = work / ARTIFACT
                identity = build_apk(
                    work / "package",
                    pending,
                    program.machine_code,
                    self.report["application"],
                    icon,
                    assets,
                    self.report,
                    tools,
                    key or Path(__file__).resolve().parent.parent / ".local/signing/debug.keystore",
                    self.log,
                )
                self.report["output"] = {**identity, "apk": ARTIFACT}
                self.transition("VALIDATING", "Independently validating signed APK and native dependencies")
                validation = validate_apk(
                    pending, tools, identity["package"], identity["entryPoint"], log=self.log
                )
                self.report["validation"] = validation
                shutil.move(str(pending), self.output / ARTIFACT)
                self.transition(
                    "READY",
                    "Signed standalone native APK passed static validation; device execution is not claimed",
                )
        except Exception as exc:
            self.report["error"] = {"type": type(exc).__name__, "message": str(exc)}
            if self.report["state"] not in ("READY", "PARTIAL", "BLOCKED", "FAILED"):
                self.transition("FAILED", str(exc))
            else:
                self.save(force=True)
        self.save(force=True)
        return self.report
