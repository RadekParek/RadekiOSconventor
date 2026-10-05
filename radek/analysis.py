"""Shared native analyzer invocation, dependency graph, fail-closed conversion proof."""

from __future__ import annotations
import json
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from .archive import InputError
from .resources import MACH_MAGICS
from .ir import Unsupported, Program, lift

ROOT = Path(__file__).resolve().parent.parent


def analyzer_path() -> Path:
    candidates = (
        [Path(os.environ["RADEK_ANALYZER"])]
        if "RADEK_ANALYZER" in os.environ
        else [ROOT / ".local/bin/radek-macho", ROOT / "native/build/radek-macho"]
    )
    for path in candidates:
        if path.is_file():
            return path
    raise RuntimeError("native analyzer missing; run python3 tools/build_native.py")


def analyze(path: Path) -> dict:
    proc = subprocess.run([str(analyzer_path()), str(path)], text=True, capture_output=True, timeout=60)
    if proc.returncode:
        raise InputError("Mach-O analysis failed: " + proc.stderr.strip())
    if len(proc.stdout) > 64 * 1024 * 1024:
        raise InputError("analysis output exceeds limit")
    return json.loads(proc.stdout)


def dependency_graph(app: Path, main: Path, report: dict) -> dict:
    """Analyze the executable plus every embedded Mach-O image in the bundle.

    Embedded images are analyzed concurrently: each one is a separate native
    analyzer process, so a bundle with many frameworks no longer pays the
    process latency serially.
    """
    nodes = [{"path": main.relative_to(app).as_posix(), "analysis": report}]
    embedded: list[Path] = []
    for path in sorted(app.rglob("*")):
        if not path.is_file() or path == main:
            continue
        with path.open("rb") as f:
            magic = f.read(4)
        if magic in MACH_MAGICS:
            embedded.append(path)
    if len(embedded) > 1:
        with ThreadPoolExecutor(max_workers=min(8, len(embedded))) as pool:
            analyses = list(pool.map(analyze, embedded))
    else:
        analyses = [analyze(path) for path in embedded]
    for path, analysis in zip(embedded, analyses):
        nodes.append({"path": path.relative_to(app).as_posix(), "analysis": analysis})
    edges = []
    by_path = {n["path"] for n in nodes}
    for node in nodes:
        for sl in node["analysis"]["slices"]:
            for dep in sl["dependencies"]:
                name = dep["path"]
                target = None
                if name.startswith("@executable_path/"):
                    candidate = name[len("@executable_path/") :]
                    if candidate in by_path:
                        target = candidate
                elif name.startswith("@loader_path/"):
                    candidate = (Path(node["path"]).parent / name[len("@loader_path/") :]).as_posix()
                    if candidate in by_path:
                        target = candidate
                elif name.startswith("@rpath/"):
                    for rpath in sl.get("rpaths", []):
                        prefix = (
                            rpath.replace("@executable_path", "").lstrip("/")
                            if rpath.startswith("@executable_path")
                            else rpath.replace("@loader_path", str(Path(node["path"]).parent))
                        )
                        candidate = (Path(prefix) / name[len("@rpath/") :]).as_posix()
                        if candidate in by_path:
                            target = candidate
                edges.append(
                    {
                        "from": node["path"],
                        "architecture": sl["architecture"],
                        "installName": name,
                        "resolvedBundlePath": target,
                        "classification": "unsupported",
                        "reason": (
                            "embedded binary ABI/linking not implemented"
                            if target
                            else "no verified Darwin framework/ABI provider"
                        ),
                    }
                )
    return {"nodes": nodes, "edges": edges}


#: Mach-O load commands the entry-only backend understands. Anything else would
#: require a loader semantic this pipeline does not implement, so it fails closed.
#: Shared with `radek.ceiling` so the gate assessment and the prover can never
#: disagree about which images are even loadable.
ALLOWED_ENTRY_LOAD_COMMANDS = frozenset(
    {
        1,
        0x19,
        2,
        0xB,
        0x1B,
        0x24,
        0x25,
        0x2F,
        0x30,
        0x32,
        0x80000028,
        0x1D,
        0x26,
        0x29,
        0x21,
        0x2C,
        0xC,
        0xD,
        0x18,
        0x20,
        0x80000018,
        0x8000001C,
        0x8000001F,
        0x80000023,
        0x22,
        0x80000022,
        0x80000033,
    }
)


def prove_leaf(
    executable: Path,
    report: dict,
    graph: dict,
    reconstruction: dict | None = None,
    target_abi: str = "auto",
) -> tuple[dict, Program]:
    if target_abi not in ("auto", "arm64-v8a", "armeabi-v7a"):
        raise Unsupported("unsupported Android target ABI: " + target_abi)
    if any(s["encrypted"] for n in graph["nodes"] for s in n["analysis"]["slices"]):
        raise Unsupported(
            "encrypted/FairPlay Mach-O: conversion is prohibited; obtain an unprotected authorized build"
        )
    if len(graph["nodes"]) != 1:
        raise Unsupported(
            "embedded frameworks/plugins require a native linker backend that is not implemented"
        )
    candidates = list(report["slices"])
    if target_abi == "arm64-v8a":
        candidates = [s for s in candidates if s["architecture"] == "arm64"]
        if not candidates:
            raise Unsupported("the selected 64-bit Android ABI requires an ARM64 IPA slice")
    elif target_abi == "armeabi-v7a":
        candidates = [s for s in candidates if s["architecture"] in ("armv7s", "armv7", "armv6")]
        if not candidates:
            raise Unsupported("the selected 32-bit Android ABI requires an ARMv6/ARMv7 IPA slice")
    order = (
        {"armv7s": 0, "armv7": 1, "armv6": 2}
        if target_abi == "armeabi-v7a"
        else {"arm64": 0, "armv7s": 1, "armv7": 2, "armv6": 3}
    )
    candidates.sort(key=lambda s: order.get(s["architecture"], 99))
    failures = []
    data = executable.read_bytes()
    for sl in candidates:
        try:
            if sl["architecture"] == "arm64e" or sl["pacRequired"]:
                raise Unsupported("ARM64e pointer authentication stripping/re-signing is not proven safe")
            if sl["architecture"] not in ("arm64", "armv7", "armv7s", "armv6") or sl["bigEndian"]:
                raise Unsupported("unsupported CPU/endian format")
            if sl["fileType"] != 2:
                raise Unsupported("entry must be an MH_EXECUTE program")
            if not sl.get("bindDecodingComplete", True):
                diagnostics = sl.get("bindDiagnostics", [])
                details = "; ".join(item.get("message", "unknown bind decoding error") for item in diagnostics)
                raise Unsupported("dyld bind table is incomplete and cannot be linked safely" + (": " + details if details else ""))
            # The standalone backend emits only the proven entry routine, not the
            # rest of this Mach-O image. Linked dylibs/import names therefore need
            # no shim when that routine has no calls, memory accesses, or address
            # references; the instruction proof below rejects every such use.
            if "chainedFixups" in sl:
                raise Unsupported("chained fixups require address reconstruction")
            if sl["metadata"]:
                raise Unsupported(
                    "Objective-C/Swift/initializers/unwind metadata requires additional runtime support"
                )
            # Data-only dyld binds/rebases and indirect symbol tables are retained
            # in the report but are not copied into this entry-only artifact. A
            # referenced pointer/call/address would have to pass the instruction
            # and entry-section checks below, where it is deliberately rejected.
            allowed_commands = ALLOWED_ENTRY_LOAD_COMMANDS
            if any(lc["command"] not in allowed_commands for lc in sl["loadCommands"]):
                raise Unsupported("load command requires an unsupported loader semantic")
            if "entryOffset" not in sl:
                raise Unsupported("only LC_MAIN entry points are currently reconstructed")
            entry = sl["entryOffset"]
            section = None
            for seg in sl["segments"]:
                for sec in seg["sections"]:
                    if (
                        sec["offset"] <= entry < sec["offset"] + sec["size"]
                        and sec["name"] == "__text"
                        and seg["initialProtection"] & 4
                    ):
                        section = sec
            if section is None:
                raise Unsupported("entry point is not in an executable __text section")
            if entry % (4 if sl["architecture"] == "arm64" else 2):
                raise Unsupported("unaligned entry point")
            address = section["address"] + entry - section["offset"]
            thumb = any(sym["value"] == address and sym["description"] & 8 for sym in sl["symbols"])
            code = data[sl["offset"] + entry : sl["offset"] + section["offset"] + section["size"]]
            target_arch = "arm64" if sl["architecture"] == "arm64" else "armv7"
            program = lift(code, sl["architecture"], thumb, target_arch=target_arch)
            code_start = entry - section["offset"]
            code_end = code_start + program.source_size
            for relocation in section["relocations"]:
                if relocation.get("scattered"):
                    raise Unsupported("scattered relocation in the entry code section is not linkable")
                relocation_start = int(relocation["address"])
                relocation_end = relocation_start + (1 << int(relocation["length"]))
                if relocation_start < code_end and code_start < relocation_end:
                    raise Unsupported("entry instructions contain relocations that require linker adaptation")
            return sl, program
        except Unsupported as exc:
            failures.append(sl["architecture"] + ": " + str(exc))
    if reconstruction:
        from .recon.report import blockers as recon_blockers

        # Only reachable APIs are blockers; a linked-but-unused framework is not.
        failures.extend(recon_blockers(reconstruction))
    raise Unsupported("; ".join(failures))


# Static components that describe a framework/runtime dependency. Their status is
# decided by reconstructed API reachability, never by the mere fact of a link.
FRAMEWORK_COMPONENTS = (
    "Foundation/CoreFoundation",
    "UIKit/CoreGraphics",
    "EAGL/OpenGL ES",
    "AudioToolbox/AVFoundation/OpenAL",
    "Swift",
    "Metal",
    "Darwin C/C++ exceptions/TLS/pthreads",
    "iOS lifecycle/input/sensors",
)

# Which static capability component each reconstructed API area belongs to.
COMPONENT_OF_AREA = {
    "graphics": "UIKit/CoreGraphics",
    "ui": "UIKit/CoreGraphics",
    "audio": "AudioToolbox/AVFoundation/OpenAL",
    "audio/video": "AudioToolbox/AVFoundation/OpenAL",
    "video": "AudioToolbox/AVFoundation/OpenAL",
    "foundation": "Foundation/CoreFoundation",
    "libc": "Foundation/CoreFoundation",
    "objc": "Objective-C",
    "language": "Swift",
    "input": "iOS lifecycle/input/sensors",
    "input/social": "iOS lifecycle/input/sensors",
    "sensors": "iOS lifecycle/input/sensors",
    "concurrency": "Darwin C/C++ exceptions/TLS/pthreads",
}

# Framework dependency that maps to each static component.
COMPONENT_OF_FRAMEWORK = {
    "Foundation": "Foundation/CoreFoundation",
    "CoreFoundation": "Foundation/CoreFoundation",
    "UIKit": "UIKit/CoreGraphics",
    "CoreGraphics": "UIKit/CoreGraphics",
    "QuartzCore": "UIKit/CoreGraphics",
    "OpenGLES": "EAGL/OpenGL ES",
    "EAGL": "EAGL/OpenGL ES",
    "Metal": "Metal",
    "AVFoundation": "AudioToolbox/AVFoundation/OpenAL",
    "AudioToolbox": "AudioToolbox/AVFoundation/OpenAL",
    "OpenAL": "AudioToolbox/AVFoundation/OpenAL",
    "Swift": "Swift",
    "libobjc": "Objective-C",
    "GameKit": "iOS lifecycle/input/sensors",
    "CoreMotion": "iOS lifecycle/input/sensors",
}


def _blank_entry() -> dict:
    return {
        "count": 0,
        "native": 0,
        "compatibility": 0,
        "blocked": 0,
        "symbols": [],
        "frameworks": [],
        "linked": {},
        "metadata": [],
    }


def _reconstructed_usage(reconstruction: dict) -> dict[str, dict]:
    """Aggregate reachable API usage per static capability component."""
    usage: dict[str, dict] = {}
    for image in reconstruction.get("images", []) or []:
        for slice_data in image.get("slices", []) or []:
            apis = slice_data.get("apis") or {}
            runtime = slice_data.get("objectiveC") or {}
            if runtime.get("classCount") or runtime.get("messageSelectorCount"):
                entry = usage.setdefault("Objective-C", _blank_entry())
                entry["metadata"].append(
                    f"{runtime.get('classCount', 0)} class(es), "
                    f"{runtime.get('selectorCount', 0)} selector(s), "
                    f"{runtime.get('messageSelectorCount', 0)} message send(s)"
                )
            swift_runtime = slice_data.get("swift") or {}
            if swift_runtime.get("typeCount") or swift_runtime.get("symbolCount"):
                entry = usage.setdefault("Swift", _blank_entry())
                entry["metadata"].append(
                    f"{swift_runtime.get('typeCount', 0)} type(s), {swift_runtime.get('symbolCount', 0)} symbol(s)"
                )
            for item in apis.get("used", []) or []:
                component = COMPONENT_OF_AREA.get(item.get("area")) or COMPONENT_OF_FRAMEWORK.get(
                    item.get("framework")
                )
                if component is None:
                    continue
                entry = usage.setdefault(component, _blank_entry())
                entry["count"] += 1
                feasibility = item.get("feasibility", "compatibility")
                entry[feasibility if feasibility in entry else "compatibility"] += 1
                if len(entry["symbols"]) < 8:
                    entry["symbols"].append(item.get("name"))
                framework = item.get("framework")
                if framework and framework not in entry["frameworks"]:
                    entry["frameworks"].append(framework)
            for name, entry in (apis.get("linkedFrameworks") or {}).items():
                component = COMPONENT_OF_FRAMEWORK.get(name)
                if component is None:
                    continue
                usage.setdefault(component, _blank_entry())["linked"][name] = entry
    return usage


def _usage_detail(component: str, usage: dict) -> dict | None:
    entry = usage.get(component)
    if entry is None:
        return None
    linked = entry.get("linked") or {}
    if not entry["count"]:
        names = ", ".join(sorted(linked)[:4])
        if entry["metadata"]:
            return {
                "component": component,
                "status": "BLOCKED",
                "detail": (
                    "metadata present ("
                    + "; ".join(entry["metadata"][:2])
                    + "); the runtime ABI is not implemented"
                ),
            }
        return {
            "component": component,
            "status": "SUPPORTED",
            "detail": (
                f"linked ({names or 'no framework'}) but no reachable API use was found in the "
                "reconstructed code; no code execution is needed for this import"
            ),
        }
    parts = [
        f"{entry['count']} reachable API(s): {entry['native']} same-name Android native candidates, "
        f"{entry['compatibility']} requiring compatibility rewrites, {entry['blocked']} unsupported"
    ]
    if entry["symbols"]:
        parts.append("e.g. " + ", ".join(entry["symbols"][:4]))
    if entry["frameworks"]:
        parts.append("frameworks: " + ", ".join(entry["frameworks"][:4]))
    status = "BLOCKED" if entry["blocked"] else "PARTIAL"
    return {"component": component, "status": status, "detail": "; ".join(parts)}


def capabilities(reconstruction: dict | None = None) -> list[dict]:
    """Capability matrix. When a reconstruction is supplied, linked-but-unused
    dependencies are no longer reported as blockers: only reachable APIs decide."""
    usage = _reconstructed_usage(reconstruction) if reconstruction else {}
    entries = [
        {
            "component": "ARM64 closed integer leaf assessment",
            "status": "PARTIAL",
            "detail": "MOVZ/MOVK, 32-bit immediate ADD/SUB and RET can be lowered in memory only; no runnable code or game APK is emitted",
        },
        {
            "component": "ARMv6/ARMv7/ARMv7s/Thumb/Thumb-2 assessment",
            "status": "PARTIAL",
            "detail": "selected immediate MOV/ADD/SUB/return subsets can be lowered in memory to ARMv7 form; no runnable code or 32-bit game APK is emitted",
        },
        {
            "component": "Objective-C",
            "status": "BLOCKED",
            "detail": "experimental host runtime tests are not Apple metadata/objc_msgSend ABI compatibility",
        },
        *[
            {
                "component": name,
                "status": "BLOCKED",
                "detail": "no verified conversion provider; dependency blocks conversion",
            }
            for name in FRAMEWORK_COMPONENTS
        ],
        {
            "component": "resources",
            "status": "PARTIAL",
            "detail": (
                "bundle paths retained; PNG (including Apple CgBI) and compiled Assets.car icons are "
                "decoded; shader/asset rewriting unsupported"
            ),
        },
        {
            "component": "complete game APK packaging",
            "status": "BLOCKED",
            "detail": "disabled until a complete game-code/API replacement backend exists; the importer APK is a separate product",
        },
    ]
    if not usage:
        return entries
    known = set(COMPONENT_OF_AREA.values()) | set(COMPONENT_OF_FRAMEWORK.values())
    for index, entry in enumerate(entries):
        if entry["component"] not in known and entry["component"] not in FRAMEWORK_COMPONENTS:
            continue
        detail = _usage_detail(entry["component"], usage)
        if detail:
            entries[index] = {**entry, "status": detail["status"], "detail": detail["detail"]}
        elif entry["component"] in FRAMEWORK_COMPONENTS:
            entries[index] = {
                **entry,
                "status": "SUPPORTED",
                "detail": "not linked by this binary; no reachable API use was reconstructed",
            }
    for component in sorted(usage):
        if component in known:
            continue
        detail = _usage_detail(component, usage)
        if detail:
            entries.append({**detail, "component": f"{component} (reconstructed usage)"})
    return entries
