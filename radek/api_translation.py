"""Generate a tiny, explicit set of Darwin time-API replacement sources.

These wrappers are implemented and host-tested in ``native/src``. This module
copies only wrappers whose callers are statically reachable from the selected
Mach-O entry through reconstructed internal calls. It does not rewrite call
sites, prove dynamic Objective-C dispatch, link the source into the translated
entry library, or claim complete APK/game coverage.
"""

from __future__ import annotations

import hashlib
from pathlib import Path


_SUPPORTED = {
    "_CFAbsoluteTimeGetCurrent": (
        "CFAbsoluteTimeGetCurrent",
        "RADEK_API_CFAbsoluteTimeGetCurrent",
    ),
    "_CACurrentMediaTime": ("CACurrentMediaTime", "RADEK_API_CACurrentMediaTime"),
    "_mach_absolute_time": ("mach_absolute_time", "RADEK_API_mach_absolute_time"),
    "_mach_timebase_info": ("mach_timebase_info", "RADEK_API_mach_timebase_info"),
}


def _as_address(value) -> int | None:
    try:
        return int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError):
        return None


def _entry_reachable_functions(slice_data: dict) -> set[str]:
    """Return reconstructed function names reachable from the selected entry.

    Internal edges are followed only when the reconstruction marks them
    non-external and resolves their target to a reconstructed function. Unknown
    edges are not guessed into reachable paths.
    """
    functions = slice_data.get("functions", []) or []
    entry = _as_address(slice_data.get("entryPoint"))
    if entry is None:
        return set()

    names = {item.get("name") for item in functions if isinstance(item, dict) and item.get("name")}
    by_address = {
        address: item["name"]
        for item in functions
        if isinstance(item, dict)
        and item.get("name")
        and (address := _as_address(item.get("address"))) is not None
    }
    roots = {by_address[entry]} if entry in by_address else set()
    if not roots:
        return set()

    adjacency: dict[str, set[str]] = {}
    for edge in ((slice_data.get("callGraph") or {}).get("edges") or []):
        if not isinstance(edge, dict) or edge.get("external") is not False:
            continue
        caller = edge.get("from")
        if caller not in names:
            continue
        callee = edge.get("to")
        if callee not in names:
            target = _as_address(edge.get("address"))
            callee = by_address.get(target)
        if callee in names:
            adjacency.setdefault(caller, set()).add(callee)

    reachable = set(roots)
    pending = list(roots)
    while pending:
        caller = pending.pop()
        for callee in adjacency.get(caller, set()):
            if callee not in reachable:
                reachable.add(callee)
                pending.append(callee)
    return reachable


def reachable_imports(reconstruction: dict) -> set[str]:
    """Collect supported imports called by a function reachable from LC_MAIN."""
    names: set[str] = set()
    for image in reconstruction.get("images", []) or []:
        for slice_data in image.get("slices", []) or []:
            callers = _entry_reachable_functions(slice_data)
            if not callers:
                continue
            for use in ((slice_data.get("apis") or {}).get("used") or []):
                if not isinstance(use, dict):
                    continue
                name = use.get("name")
                use_callers = use.get("callers") or []
                if isinstance(name, str) and name in _SUPPORTED and any(caller in callers for caller in use_callers):
                    names.add(name)
    return names


def generate(reconstruction: dict, output: Path) -> dict:
    """Write compilable replacement source for exact supported entry-reachable APIs."""
    selected = sorted(reachable_imports(reconstruction))
    if not selected:
        return {
            "status": "NO_ENTRY_REACHABLE_IMPLEMENTED_API",
            "attempted": False,
            "generatedApiReplacements": 0,
            "linkedApiReplacements": 0,
            "totalReachableApiCount": 0,
            "untranslatedReachableApiCount": 0,
            "codeGenerated": False,
            "completeGameConversion": False,
            "message": (
                "No reconstructed call from the selected Mach-O entry reached the small implemented "
                "time-API subset; name matches and semantic targets remain analysis-only."
            ),
            "replacements": [],
        }

    source_root = Path(__file__).resolve().parent.parent / "native"
    source_dir = output / "api-replacements"
    source_dir.mkdir(parents=True, exist_ok=True)
    implementation = source_root / "src" / "apple_time_compat.cpp"
    header = source_root / "include" / "apple_time_compat.h"
    header_copy = source_dir / "apple_time_compat.h"
    header_copy.write_bytes(header.read_bytes())

    defines = [
        "#define RADEK_API_REPLACEMENTS_ONLY 1",
        *[f"#define {_SUPPORTED[name][1]} 1" for name in selected],
        "",
    ]
    generated_source = source_dir / "api-replacements.cpp"
    generated_source.write_text(
        "\n".join(defines) + implementation.read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    source_hash = hashlib.sha256(generated_source.read_bytes()).hexdigest()

    replacements = []
    for source_symbol in selected:
        target_symbol, _ = _SUPPORTED[source_symbol]
        replacements.append(
            {
                "sourceSymbol": source_symbol,
                "targetAndroidApi": f"libioscompat.so:{target_symbol}",
                "targetSymbol": target_symbol,
                "implementationArtifact": "api-replacements/api-replacements.cpp",
                "implementationSha256": source_hash,
                "codeGenerated": True,
                "linkedIntoGame": False,
                "linkedIntoApk": False,
                "reachableInSourceImage": True,
                "reachableFromEntry": True,
            }
        )

    return {
        "status": "IMPLEMENTATIONS_GENERATED_NOT_LINKED",
        "attempted": True,
        "generatedApiReplacements": len(replacements),
        "linkedApiReplacements": 0,
        "totalReachableApiCount": len(selected),
        "untranslatedReachableApiCount": 0,
        "codeGenerated": True,
        "sourcePath": "api-replacements/api-replacements.cpp",
        "headerPath": "api-replacements/apple_time_compat.h",
        "implementationTemplate": "native/src/apple_time_compat.cpp",
        "completeGameConversion": False,
        "message": (
            f"Generated {len(replacements)} real time-API replacement implementation(s) from entry-reachable "
            "imports. The sources are not linked into the translated entry library or an APK."
        ),
        "replacements": replacements,
    }
