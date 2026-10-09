"""Bounded host-side static-recompilation plan for one selected Mach-O slice.

The bounded complete-game prover accepts only an executable that is *exactly one*
closed-integer routine. Without a verified Android link, ``portProgress.percent``
remains zero; a host-only plan or portable-C translation must not inflate it.

This module answers the narrower, measurable question: *how much of this slice's
executable ``__text`` does the host static recompiler actually translate into
portable code?* It runs the same lifter the differential test proves
(:mod:`radek.game.lift`) over every discovered function, counts the unique source
instruction bytes emitted inside ``__text``, and reports that host-only coverage
separately from Android-linked progress, together with its limits.

What this number is and is not:

* it **is** static-recompiled source bytes of the game's own code, produced by the
  repo's fail-closed emitter (a function either lifts completely or raises);
* it is **not** an Android artifact: nothing here links a game, writes an APK,
  supplies framework/API behavior, or runs on a device;
* it is **not** gameplay progress, and it never counts symbol triage, stub
  handlers or name candidates.

The pass is optional and bounded. Without the ``capstone`` package (the analyzer
used by the emitter) it reports ``UNAVAILABLE`` and leaves every other report
field untouched. It is also time-bounded: when the budget runs out the result is
marked ``TRUNCATED`` and its percentage always states the processed subset.
"""

from __future__ import annotations

import time
from pathlib import Path

#: Default wall-clock budget for one plan pass.
DEFAULT_TIME_BUDGET_SECONDS = 90.0
#: Inputs smaller than this are not worth a separate lifter pass.
MINIMUM_TEXT_BYTES = 4096

LIMITATIONS = (
    "Statically recompiled source bytes only: no game APK, launcher, lifecycle, "
    "resources or API replacement is linked, and nothing runs on a device.",
    "Coverage is measured over functions discovered by the bounded disassembler; "
    "bytes outside any discovered function are not claimed.",
    "Imports are emitted as compatibility-runtime shims; a lifted function does "
    "not mean the imported Apple API it calls is implemented.",
    "Not gameplay, not a playability score, and never counted towards "
    "conversionProgress.",
)


def _unavailable(reason: str) -> dict:
    return {
        "status": "UNAVAILABLE",
        "metric": "statically recompiled function source bytes / executable __text bytes",
        "percent": 0,
        "reason": reason,
        "limitations": list(LIMITATIONS),
    }


def plan_coverage(
    executable: bytes | Path,
    *,
    time_budget: float = DEFAULT_TIME_BUDGET_SECONDS,
) -> dict:
    """Lift every discovered function of one Mach-O slice and report coverage.

    Returns a JSON-serializable dict; never raises for an unsupported or
    unreadable input, so an analysis report can always include the result.
    """
    if isinstance(executable, Path):
        try:
            data = executable.read_bytes()
        except OSError as exc:
            return _unavailable(f"executable could not be read: {exc}")
    else:
        data = bytes(executable)
    if not data:
        return _unavailable("executable is empty")

    try:
        from .game import disasm, lift, macho
    except Exception as exc:  # pragma: no cover - import-time environment failure
        return _unavailable(f"host lifter is not importable: {exc}")

    try:
        require_capstone = getattr(disasm, "require_capstone", None)
        if require_capstone is not None:
            require_capstone()
    except Exception as exc:
        return _unavailable(
            "the capstone package is required for the host static-recompilation plan: "
            f"{exc}"
        )

    started = time.monotonic()
    try:
        image = macho.parse(data)
    except Exception as exc:
        message = str(exc)
        if "32-bit" in message:
            return _unavailable(
                "the game lifter statically recompiles 32-bit ARM (ARMv6/ARMv7) slices only; "
                f"this image is not one ({message})"
            )
        return _unavailable(f"input is not a parseable classic 32-bit Mach-O image: {message}")

    text_section = image.section_named("__TEXT", "__text")
    text_bytes = int(getattr(text_section, "size", 0) or 0)
    if text_bytes < MINIMUM_TEXT_BYTES:
        return _unavailable(
            f"executable __text is {text_bytes} byte(s); below the {MINIMUM_TEXT_BYTES}-byte plan threshold"
        )

    try:
        functions = disasm.disassemble_all(image)
        context = lift.build_context(image, functions)
    except Exception as exc:
        return _unavailable(f"the bounded disassembler could not decode this input: {exc}")

    discovered_addresses: list[int] = []
    recompiled_addresses: list[int] = []
    recompiled = 0
    failed = 0
    skipped_loader_glue = 0
    truncated = False
    skip_names = getattr(lift, "SKIP_NAMES", frozenset())
    for address in sorted(functions):
        function = functions[address]
        if time.monotonic() - started > time_budget:
            truncated = True
            break
        # Loader glue is not recompiled by the emitter; the translated code calls
        # compatibility shims directly. Those functions are excluded from both
        # sides of the ratio, exactly as the code generator excludes them.
        if function.name in skip_names or address in context.import_of_stub:
            skipped_loader_glue += 1
            continue
        discovered_addresses.append(address)
        try:
            lift.lift_function(context, function)
        except lift.LiftError:
            failed += 1
            continue
        recompiled += 1
        recompiled_addresses.append(address)

    # Count the actual decoded instruction-byte intervals inside __text. Function
    # symbols can overlap, so summing per-function sizes would double-count the
    # same source bytes and can inflate host-only coverage beyond the section.
    from .game.codegen import _translated_text_coverage

    text_address = int(getattr(text_section, "address", 0) or 0)
    discovered_coverage = _translated_text_coverage(
        functions, discovered_addresses, text_address, text_bytes
    )
    recompiled_coverage = _translated_text_coverage(
        functions, recompiled_addresses, text_address, text_bytes
    )
    discovered_bytes = discovered_coverage["uniqueTextBytes"]
    recompiled_bytes = recompiled_coverage["uniqueTextBytes"]

    percent = 0.0
    if text_bytes > 0:
        percent = round(min(100.0, 100.0 * recompiled_bytes / text_bytes), 6)
    seconds = round(time.monotonic() - started, 3)
    return {
        "status": "TRUNCATED" if truncated else "COMPUTED",
        "metric": "statically recompiled function source bytes / executable __text bytes",
        "percent": percent,
        "functionsDiscovered": len(functions),
        "functionsStaticallyRecompiled": recompiled,
        "functionsNotRecompiled": failed,
        "discoveredFunctionBytes": discovered_bytes,
        "staticallyRecompiledBytes": recompiled_bytes,
        "discoveredUniqueTextBytes": discovered_bytes,
        "staticallyRecompiledUniqueTextBytes": recompiled_bytes,
        "summedDiscoveredFunctionInstructionBytes": discovered_coverage["summedFunctionInstructionBytes"],
        "summedDiscoveredTextInstructionBytes": discovered_coverage["summedTextInstructionBytes"],
        "overlappingDiscoveredTextInstructionBytes": discovered_coverage["overlappingTextInstructionBytes"],
        "summedRecompiledFunctionInstructionBytes": recompiled_coverage["summedFunctionInstructionBytes"],
        "summedRecompiledTextInstructionBytes": recompiled_coverage["summedTextInstructionBytes"],
        "overlappingRecompiledTextInstructionBytes": recompiled_coverage["overlappingTextInstructionBytes"],
        "executableTextBytes": text_bytes,
        "processedFunctions": recompiled + failed,
        "skippedLoaderGlueFunctions": skipped_loader_glue,
        "truncated": truncated,
        "timeBudgetSeconds": time_budget,
        "seconds": seconds,
        "implementation": "radek.game.lift over the bounded disassembler; fail-closed per function",
        "linkedIntoGame": False,
        "codeGeneratedOnDevice": False,
        "completeGameConversion": False,
        "countsAsConversionProgress": False,
        "basis": (
            f"{recompiled} discovered function(s) covering {recompiled_bytes} byte(s) of the selected slice's "
            f"executable __text ({percent}% of {text_bytes} byte(s)) are statically recompiled into portable C "
            "sources by the host lifter. No APK was assembled from them and no game is linked: this is host "
            "static-recompilation coverage of the game's own code, not gameplay or packaging progress."
        ),
        "limitations": list(LIMITATIONS),
    }
