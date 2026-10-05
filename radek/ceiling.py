"""How far the bounded converter can actually proceed for one IPA.

The pipeline is fail-closed: it refuses to emit anything unless every stage
succeeds. That is correct, but a plain ``BLOCKED`` state does not say *where*
conversion stopped or what the first real obstacle was. This module walks the
same stages in the same order as the prover (`radek.analysis.prove_leaf`) and
records one gate per stage:

``PASS``
    the stage's preconditions were verified for this input;
``BLOCKED``
    this is the ceiling: the stage cannot be passed with the code that exists;
``NOT_REACHED``
    the stage is only attempted after every earlier gate passed, so no claim is
    made about it.

Only the first blocked gate is a ceiling. Later gates are deliberately reported
as ``NOT_REACHED`` instead of being estimated, so this file can never imply
static recompilation progress that was not earned. Stub handlers, name candidates and
symbol triage are never counted as passed stages.
"""

from __future__ import annotations

from typing import Any

from .analysis import ALLOWED_ENTRY_LOAD_COMMANDS
from .compat_layer import classify as classify_import
from .ir import Unsupported, lift

CONTRACT = "conversion-ceiling-v1"

#: Ordered gates. The order matches `radek.analysis.prove_leaf`.
GATES: tuple[tuple[str, str], ...] = (
    ("TARGET_ABI", "Supported ARM slice for the requested Android ABI"),
    ("PROTECTION", "Input is not encrypted/FairPlay protected"),
    ("CONTAINER", "Single Mach-O image without embedded binaries to link"),
    ("SLICE_FORMAT", "CPU, subtype, endianness, file type and alignment are convertible"),
    ("LOADER_METADATA", "Load commands, bind table, fixups and runtime metadata are understood"),
    ("ENTRY_ROUTINE", "Entry is a provable closed-integer routine covering its __text section"),
    ("REACHABLE_CODE", "Reachable code is statically recompiled, not only the entry leaf"),
    ("API_LINKING", "Every reachable import has a linked implementation"),
    ("RESOURCES", "Bundle resources are carried into the APK"),
    ("LIFECYCLE", "Launcher/lifecycle replacement exists for the statically recompiled code"),
    ("APK_PACKAGING", "A signed, installable APK was built and statically validated"),
)

GATE_TITLES = dict(GATES)

_MESSAGE = (
    "Pre-flight assessment of how far this input can be taken towards a complete-game APK. "
    "It is not converted code, not a playability score, and it never counts name candidates, "
    "stub handlers or symbol triage as a passed stage. Passing the first six gates only means "
    "the isolated entry-leaf artifact can be produced; that artifact is not a game."
)

#: Gates that must pass before the standalone entry-leaf artifact can be produced.
_ENTRY_LEAF_GATES = ("TARGET_ABI", "PROTECTION", "CONTAINER", "SLICE_FORMAT", "LOADER_METADATA", "ENTRY_ROUTINE")


def _gate(gate_id: str, status: str, detail: str, evidence: dict | None = None) -> dict:
    return {
        "gate": gate_id,
        "title": GATE_TITLES[gate_id],
        "status": status,
        "detail": detail,
        "evidence": evidence or {},
    }


def _candidates(report: dict, target_abi: str) -> tuple[list[dict], str | None]:
    """Slice candidates in the prover's preference order, or a blocker message."""
    order = (
        {"armv7s": 0, "armv7": 1, "armv6": 2}
        if target_abi == "armeabi-v7a"
        else {"arm64": 0, "armv7s": 1, "armv7": 2, "armv6": 3}
    )
    slices = list(report.get("slices", []))
    if target_abi == "arm64-v8a":
        slices = [s for s in slices if s.get("architecture") == "arm64"]
        if not slices:
            return [], "the selected 64-bit Android ABI requires an ARM64 IPA slice"
    elif target_abi == "armeabi-v7a":
        slices = [s for s in slices if s.get("architecture") in ("armv7s", "armv7", "armv6")]
        if not slices:
            return [], "the selected 32-bit Android ABI requires an ARMv6/ARMv7 IPA slice"
    slices.sort(key=lambda s: order.get(s.get("architecture", ""), 99))
    return slices, None


#: Per-slice check order, matching `radek.analysis.prove_leaf` exactly. A slice
#: that fails gate *i* also never reaches gate *i+1*, so the deepest failure over
#: all candidate slices decides each gate's status.
_SLICE_GATE_ORDER = ("SLICE_FORMAT", "LOADER_METADATA", "ENTRY_ROUTINE", "REACHABLE_CODE")


def _slice_first_failure(slice_data: dict, executable_data: bytes | None) -> tuple[str | None, str | None]:
    """First failing per-slice check -> (gate, reason), or (None, None) when it passes.

    The checks run in exactly the order `prove_leaf` performs them, so the
    reported gate is the same statement that makes the prover move on.
    """
    architecture = slice_data.get("architecture", "")
    if architecture == "arm64e" or slice_data.get("pacRequired"):
        return "SLICE_FORMAT", "ARM64e pointer authentication stripping/re-signing is not proven safe"
    if architecture not in ("arm64", "armv7", "armv7s", "armv6") or slice_data.get("bigEndian"):
        return "SLICE_FORMAT", "unsupported CPU/endian format"
    if slice_data.get("fileType") != 2:
        return "SLICE_FORMAT", "entry must be an MH_EXECUTE program"
    if slice_data.get("bindDecodingComplete") is False:
        diagnostics = slice_data.get("bindDiagnostics", [])
        details = "; ".join(item.get("message", "unknown bind decoding error") for item in diagnostics)
        return (
            "LOADER_METADATA",
            "dyld bind table is incomplete and cannot be linked safely" + (": " + details if details else ""),
        )
    if "chainedFixups" in slice_data:
        return "LOADER_METADATA", "chained fixups require address reconstruction"
    if slice_data.get("metadata"):
        return (
            "LOADER_METADATA",
            "Objective-C/Swift/initializers/unwind metadata requires additional runtime support",
        )
    if any(lc.get("command") not in ALLOWED_ENTRY_LOAD_COMMANDS for lc in slice_data.get("loadCommands", [])):
        return "LOADER_METADATA", "load command requires an unsupported loader semantic"
    if "entryOffset" not in slice_data:
        return "LOADER_METADATA", "only LC_MAIN entry points are currently reconstructed"
    entry = slice_data["entryOffset"]
    section = None
    for segment in slice_data.get("segments", []):
        for candidate in segment.get("sections", []):
            candidate_start = int(candidate.get("offset", 0))
            candidate_end = candidate_start + int(candidate.get("size", 0))
            if (
                candidate_start <= entry < candidate_end
                and candidate.get("name") == "__text"
                and int(segment.get("initialProtection", 0)) & 4
            ):
                section = candidate
    if section is None:
        return "ENTRY_ROUTINE", "entry point is not in an executable __text section"
    if entry % (4 if architecture == "arm64" else 2):
        return "SLICE_FORMAT", "unaligned entry point"
    if executable_data is None:
        return "ENTRY_ROUTINE", "executable bytes are unavailable, so the instruction proof cannot run"
    address = int(section.get("address", 0)) + entry - int(section.get("offset", 0))
    thumb = any(
        sym.get("value") == address and sym.get("description", 0) & 8 for sym in slice_data.get("symbols", [])
    )
    code_start_offset = slice_data["offset"] + entry
    code_end_offset = slice_data["offset"] + int(section.get("offset", 0)) + int(section.get("size", 0))
    code = executable_data[code_start_offset:code_end_offset]
    try:
        program = lift(code, architecture, thumb, target_arch="arm64" if architecture == "arm64" else "armv7")
    except Unsupported as exc:
        return "ENTRY_ROUTINE", str(exc)
    code_start = entry - int(section.get("offset", 0))
    code_end = code_start + program.source_size
    for relocation in section.get("relocations", []):
        if relocation.get("scattered"):
            return "ENTRY_ROUTINE", "scattered relocation in the entry code section is not linkable"
        relocation_start = int(relocation.get("address", 0))
        relocation_end = relocation_start + (1 << int(relocation.get("length", 0)))
        if relocation_start < code_end and code_start < relocation_end:
            return "ENTRY_ROUTINE", "entry instructions contain relocations that require linker adaptation"
    if entry != int(section.get("offset", 0)) or program.source_size != int(section.get("size", 0)):
        return (
            "REACHABLE_CODE",
            "the proven entry routine does not cover the whole executable __text section: "
            f"{program.source_size} of {int(section.get('size', 0))} byte(s)",
        )
    return None, None


def _selected_slice(reconstruction: dict, architecture: str) -> dict | None:
    for image in reconstruction.get("images", []):
        for slice_data in image.get("slices", []):
            if slice_data.get("architecture") == architecture:
                return slice_data
    return None


def _reachable_imports(reconstruction: dict) -> list[dict]:
    used: list[dict] = []
    for image in reconstruction.get("images", []):
        for slice_data in image.get("slices", []):
            apis = slice_data.get("apis") or {}
            for item in apis.get("used", []):
                if isinstance(item, dict) and item.get("name"):
                    used.append(item)
    return used


def assess(
    mach_report: dict,
    graph: dict,
    reconstruction: dict,
    target_abi: str = "auto",
    executable_data: bytes | None = None,
    apk_outcome: dict | None = None,
    prover_message: str | None = None,
) -> dict:
    """Build the ordered gate ledger and name the ceiling gate."""
    gates: list[dict] = []
    ceiling_gate: str | None = None
    ceiling_reason: str | None = None
    decided = False

    def record(gate_id: str, ok: bool, detail: str, evidence: dict | None = None) -> None:
        nonlocal ceiling_gate, ceiling_reason, decided
        if decided:
            gates.append(_gate(gate_id, "NOT_REACHED", "Only attempted after every earlier gate passes."))
            return
        if ok:
            gates.append(_gate(gate_id, "PASS", detail, evidence))
            return
        decided = True
        ceiling_gate = gate_id
        ceiling_reason = detail
        gates.append(_gate(gate_id, "BLOCKED", detail, evidence))

    # --- container level -------------------------------------------------
    conversion_candidates, abi_blocker = _candidates(mach_report, target_abi)
    record(
        "TARGET_ABI",
        abi_blocker is None,
        abi_blocker
        or (
            f"{len(conversion_candidates)} convertible slice(s) for target ABI {target_abi}: "
            + ", ".join(s.get("architecture", "?") for s in conversion_candidates)
        ),
        {
            "requestedAbi": target_abi,
            "candidateArchitectures": [s.get("architecture") for s in conversion_candidates],
        },
    )

    any_encrypted = any(
        s.get("encrypted") for node in graph.get("nodes", []) for s in node.get("analysis", {}).get("slices", [])
    )
    record(
        "PROTECTION",
        not any_encrypted,
        "no encrypted slice present"
        if not any_encrypted
        else "encrypted/FairPlay Mach-O: conversion is prohibited; obtain an unprotected authorized build",
    )

    node_count = len(graph.get("nodes", []))
    record(
        "CONTAINER",
        node_count == 1,
        f"{node_count} Mach-O image(s) to link"
        if node_count == 1
        else "embedded frameworks/plugins require a native linker backend that is not implemented",
        {"imageCount": node_count},
    )

    # --- per-slice preconditions ----------------------------------------
    slice_failures: list[dict] = []
    best: dict | None = None
    for slice_data in conversion_candidates:
        gate_id, reason = _slice_first_failure(slice_data, executable_data)
        progress = _SLICE_GATE_ORDER.index(gate_id) if gate_id else len(_SLICE_GATE_ORDER)
        attempt = {
            "architecture": slice_data.get("architecture"),
            "gate": gate_id,
            "reason": reason,
            "depth": progress,
        }
        slice_failures.append(attempt)
        if best is None or progress > best["depth"]:
            best = attempt

    convertible = best if best is not None and best["depth"] == len(_SLICE_GATE_ORDER) else None
    best_depth = best["depth"] if best else -1
    best_detail = best["reason"] if best else "no candidate slice"
    deepest_arch = (best or {}).get("architecture", "?")
    evidence = {"candidateFailures": slice_failures}

    def slice_record(gate_id: str, pass_detail: str) -> None:
        depth = _SLICE_GATE_ORDER.index(gate_id)
        if convertible is not None:
            record(gate_id, True, pass_detail, evidence)
        elif best_depth > depth:
            # A deeper gate failed, so this check did pass for the furthest slice.
            record(
                gate_id,
                True,
                f"slice {deepest_arch} passes this check; the ceiling is later ({best.get('gate')})",
                evidence,
            )
        else:
            record(gate_id, False, best_detail, evidence)

    slice_record(
        "SLICE_FORMAT",
        f"slice {deepest_arch} passes CPU/subtype/endianness/file-type/alignment checks",
    )
    slice_record(
        "LOADER_METADATA",
        f"load commands, bind table and metadata of slice {deepest_arch} are understood",
    )
    slice_record("ENTRY_ROUTINE", f"entry of slice {deepest_arch} passes the closed-integer proof")

    # --- reachable code, APIs, resources, lifecycle ----------------------
    stats: dict[str, Any] = {}
    if convertible is not None:
        selected = _selected_slice(reconstruction, convertible.get("architecture", ""))
        stats = (selected or {}).get("disassembly") or {}
    functions = int(stats.get("functions", 0))
    text_bytes = int(stats.get("textBytes", 0))
    recompiled_bytes = 0
    if convertible is not None:
        for segment in convertible.get("segments", []):
            for section in segment.get("sections", []):
                if section.get("name") == "__text" and int(segment.get("initialProtection", 0)) & 4:
                    recompiled_bytes = int(section.get("size", 0))
    percent = round(100.0 * recompiled_bytes / text_bytes, 6) if text_bytes else 0.0
    single_function = functions == 1
    fully_recompiled = convertible is not None and single_function
    coverage_failed = best_depth == _SLICE_GATE_ORDER.index("REACHABLE_CODE")
    if fully_recompiled:
        reachable_detail = (
            f"the single reachable function is statically recompiled in full ({recompiled_bytes} of {text_bytes} __text byte(s))"
        )
    elif coverage_failed:
        reachable_detail = best_detail
    elif convertible is None:
        reachable_detail = (
            f"{functions} reachable function(s) were recovered; general reachable-function static recompilation is not implemented"
        )
    else:
        reachable_detail = (
            f"{functions} reachable function(s) were recovered; {recompiled_bytes} of {text_bytes} __text byte(s) "
            "have a statically recompiled backend (general reachable-function static recompilation is not implemented)"
        )
    record(
        "REACHABLE_CODE",
        fully_recompiled,
        reachable_detail,
        {
            "reachableFunctions": functions,
            "recompiledBytes": recompiled_bytes,
            "totalTextBytes": text_bytes,
            "recompiledPercent": percent,
        },
    )

    imports: list = []
    if convertible is not None:
        for slice_data in mach_report.get("slices", []):
            if slice_data.get("architecture") == convertible.get("architecture"):
                imports = list(slice_data.get("imports", []))
                break
    used = _reachable_imports(reconstruction)
    verified = [u["name"] for u in used if classify_import(u["name"]) == "verified"]
    stubbed = [u["name"] for u in used if classify_import(u["name"]) == "stubbed"]
    blocked_feasibility = [
        u["name"] for u in used if (u.get("feasibility") or "blocked") == "blocked"
    ]
    no_imports = not imports
    if no_imports:
        api_detail = "the executable declares no imports, so no API binding has to be linked"
    elif not used:
        api_detail = (
            f"{len(imports)} import symbol(s) are declared by the Mach-O image; none is reached by a "
            "reconstructed call, but a complete-game build still requires the import table to be "
            "resolvable or absent"
        )
    else:
        api_detail = (
            f"{len(imports)} declared import(s), {len(used)} reached by reconstructed calls: "
            f"{len(verified)} have a host-tested implementation available but none is linked into a game, "
            f"{len(stubbed)} resolve to explicit unimplemented stubs, "
            f"{len(blocked_feasibility)} have no identified Android target"
        )
    record(
        "API_LINKING",
        no_imports,
        api_detail,
        {
            "declaredImports": len(imports),
            "reachableImports": len(used),
            "hostTestedImplementationsAvailable": len(verified),
            "stubbedUnimplemented": len(stubbed),
            "noAndroidTarget": len(blocked_feasibility),
            "examples": [u["name"] for u in used[:8]],
        },
    )

    resources_ok = convertible is not None
    record(
        "RESOURCES",
        resources_ok,
        (
            "static bundle resources are packaged verbatim with a hashed inventory in the bounded path"
            if resources_ok
            else "resource packaging is only reached once the code path converts"
        ),
    )
    record(
        "LIFECYCLE",
        resources_ok,
        (
            "the bounded launcher drives the single statically recompiled routine through JNI"
            if resources_ok
            else "no replacement exists for UIKit/UIApplicationMain or an app lifecycle of the original game"
        ),
    )

    # --- APK packaging ---------------------------------------------------
    if decided:
        gates.append(
            _gate("APK_PACKAGING", "NOT_REACHED", "Only attempted after every earlier gate passes.")
        )
    elif not (apk_outcome or {}).get("status"):
        gates.append(
            _gate(
                "APK_PACKAGING",
                "NOT_REACHED",
                "All previous gates pass; packaging runs through the real build and validation steps.",
            )
        )
    else:
        built = apk_outcome.get("status") == "BUILT"
        gates.append(
            _gate(
                "APK_PACKAGING",
                "PASS" if built else "BLOCKED",
                apk_outcome.get("detail", ""),
                {"status": apk_outcome.get("status")},
            )
        )
        if not built and ceiling_gate is None:
            ceiling_gate = "APK_PACKAGING"
            ceiling_reason = apk_outcome.get("detail", "")

    passed = sum(1 for g in gates if g["status"] == "PASS")
    ceiling = next((g for g in gates if g["status"] == "BLOCKED"), None)
    by_id = {g["gate"]: g for g in gates}
    entry_leaf = all(by_id.get(name, {}).get("status") == "PASS" for name in _ENTRY_LEAF_GATES)
    result = {
        "schemaVersion": 1,
        "contract": CONTRACT,
        "status": "ASSESSED",
        "targetAbi": target_abi,
        "gatesPassed": passed,
        "gatesTotal": len(GATES),
        "ceilingGate": ceiling_gate,
        "ceilingReason": ceiling_reason,
        "reachedGate": gates[passed - 1]["gate"] if passed else None,
        "gates": gates,
        "boundedEntryArtifact": {
            "available": entry_leaf,
            "detail": (
                "the standalone entry-leaf artifact (recompiled-entry.bin / librecompiled-entry.so) can be "
                "produced for the proven routine; it is one function, not a game, and is not linked into an APK"
                if entry_leaf
                else "no standalone entry-leaf artifact is produced for this input"
            ),
        },
        "countsAsConversionProgress": False,
        "completeGameConversion": bool(apk_outcome and apk_outcome.get("status") == "BUILT"),
        "message": _MESSAGE,
    }
    if ceiling is None and apk_outcome and apk_outcome.get("status") == "BUILT":
        result["status"] = "FULLY_CONVERTIBLE"
        result["ceilingReason"] = None
    elif ceiling is None:
        result["status"] = "PENDING_BUILD"
    if prover_message:
        result["prover"] = {
            "message": prover_message,
            "agreesWithCeiling": _prover_agrees(ceiling_reason, prover_message),
        }
    return result


def _resummarize(ceiling: dict) -> dict:
    """Recompute counts after a gate was finalized."""
    gates = ceiling.get("gates", [])
    ceiling["gatesPassed"] = sum(1 for g in gates if g.get("status") == "PASS")
    blocked = next((g for g in gates if g.get("status") == "BLOCKED"), None)
    ceiling["ceilingGate"] = blocked.get("gate") if blocked else None
    ceiling["ceilingReason"] = blocked.get("detail") if blocked else None
    ceiling["reachedGate"] = (
        gates[ceiling["gatesPassed"] - 1].get("gate") if ceiling["gatesPassed"] else None
    )
    if blocked is None and ceiling.get("completeGameConversion"):
        ceiling["status"] = "FULLY_CONVERTIBLE"
    elif blocked is None:
        ceiling["status"] = "PENDING_BUILD"
    else:
        ceiling["status"] = "ASSESSED"
    return ceiling


def attach_prover(ceiling: dict | None, prover_message: str) -> dict | None:
    """Record what the fail-closed prover said and whether the ledger agrees."""
    if not ceiling:
        return ceiling
    ceiling["prover"] = {
        "message": prover_message,
        "agreesWithCeiling": _prover_agrees(ceiling.get("ceilingReason"), prover_message),
    }
    return ceiling


def finalize_apk(ceiling: dict | None, status: str, detail: str) -> dict | None:
    """Resolve the final gate once the build was actually attempted."""
    if not ceiling:
        return ceiling
    gates = ceiling.get("gates", [])
    if not gates:
        return ceiling
    gates[-1] = _gate("APK_PACKAGING", "PASS" if status == "BUILT" else "BLOCKED", detail, {"status": status})
    ceiling["completeGameConversion"] = status == "BUILT"
    return _resummarize(ceiling)


def _prover_agrees(ceiling_reason: str | None, prover_message: str) -> bool:
    """True when the fail-closed prover stopped at the same statement as the ledger."""
    if not ceiling_reason:
        return False
    probe = ceiling_reason.split(":", 1)[0].strip()
    return bool(probe) and probe in prover_message


def markdown(ceiling: dict) -> str:
    """Human readable rendering of the gate ledger."""
    if not ceiling:
        return ""
    lines = [
        "## Conversion ceiling",
        "",
        ceiling.get("message", ""),
        "",
        f"- gates passed: **{ceiling.get('gatesPassed')} / {ceiling.get('gatesTotal')}**",
        f"- ceiling gate: **{ceiling.get('ceilingGate') or 'none'}**"
        + (f" — {ceiling.get('ceilingReason')}" if ceiling.get("ceilingReason") else ""),
        f"- counts as conversion progress: {ceiling.get('countsAsConversionProgress')}",
        "",
        "| Gate | Status | Detail |",
        "|---|---|---|",
    ]
    for gate in ceiling.get("gates", []):
        detail = str(gate.get("detail", "")).replace("\n", " ")
        if len(detail) > 220:
            detail = detail[:219] + "…"
        lines.append(f"| `{gate.get('gate')}` | {gate.get('status')} | {detail} |")
    lines.append("")
    return "\n".join(lines)
