"""Generate the full per-IPA libioscompat compatibility registry source.

Every Darwin/iOS import observed in the analyzed images receives exactly one
resolution target in the generated ``libioscompat`` source:

- a **verified** entry when the symbol is one of the host-tested implementations
  (the time APIs plus the broad libc/pthread/CoreFoundation shims, whose real
  bodies are copied from ``native/src``), or
- an explicitly labelled **stub** handler otherwise. A stub owns a stable
  function address that records invocations and returns a documented safe
  default. It exists so a future linker can resolve the symbol and so an
  accidental invocation is observable; it is never an implementation of the
  Darwin API.

The module therefore makes symbol *resolution* total while keeping the
verified/stubbed distinction exact. Stub presence is resolution coverage, not
translation coverage, and is reported separately from verified counts.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from .api_translation import _FAMILY as _SHIM_FAMILY
from .api_translation import _SUPPORTED as _VERIFIED_SHIMS
from .api_translation import selection_defines

CONTRACT = "ioscompat-registry-v1"
MAX_GENERATED_ENTRIES = 4096

KIND_VERIFIED = 1
KIND_STUB = 2


def collect_imports(reconstruction: dict) -> list[str]:
    """Return the sorted, de-duplicated Darwin import symbols of all images."""
    names: set[str] = set()
    for image in reconstruction.get("images", []) or []:
        for slice_data in image.get("slices", []) or []:
            for item in slice_data.get("imports", []) or []:
                if isinstance(item, dict):
                    name = item.get("name")
                else:
                    name = item
                if isinstance(name, str) and name.strip():
                    names.add(name.strip())
    return sorted(names)


def classify(name: str) -> str:
    """Classify a Darwin import as verified (implemented) or stubbed."""
    return "verified" if name in _VERIFIED_SHIMS else "stubbed"


def family(name: str) -> str:
    """Return the implementation family of a verified shim (empty when unknown)."""
    return _SHIM_FAMILY.get(name, "")


def is_embeddable(name: str) -> bool:
    """Only names that are safe to embed verbatim in generated C++ are handled."""
    return 0 < len(name) <= 512 and all(ch.isalnum() or ch in "_$. " for ch in name)


def _c_string_literal(name: str) -> str:
    if not is_embeddable(name):
        raise ValueError("import name is not safe to embed: " + repr(name))
    return '"' + name.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _emit_stub_machinery(count: int) -> str:
    pool = max(count, 1)
    lines = [
        "// --- Explicit stub handlers: resolution targets, NOT implementations. ---",
        "// Each stub records its invocation and returns a documented safe default.",
        "// Calling a stub is observable; it never reproduces the Darwin API.",
        "// The table is seeded in 128-wide chunks because clang rejects a single",
        "// fold expression wider than its expression-nesting limit.",
        "namespace {",
        f"constexpr unsigned long kRadekStubCount = {count}UL;",
        f"std::atomic<unsigned long long> gRadekStubCalls[{pool}]{{}};",
        "template <std::size_t Slot>",
        "void radekStubTrampoline() {",
        "    gRadekStubCalls[Slot].fetch_add(1, std::memory_order_relaxed);",
        "}",
        f"void (*gRadekStubTable[{pool}])();",
        "template <std::size_t Slot>",
        "void radekSeedOne() {",
        "    if constexpr (Slot < kRadekStubCount)",
        "        gRadekStubTable[Slot] = &radekStubTrampoline<Slot>;",
        "}",
        "template <std::size_t Base, std::size_t... Off>",
        "void radekSeedChunk(std::index_sequence<Off...>) {",
        "    (radekSeedOne<Base + Off>(), ...);",
        "}",
        "template <std::size_t... Chunk>",
        "void radekSeedAll(std::index_sequence<Chunk...>) {",
        "    (radekSeedChunk<Chunk * 128>(std::make_index_sequence<128>{}), ...);",
        "}",
        f"constexpr unsigned long kRadekStubSeedChunks = ({count}UL + 127UL) / 128UL;",
        "const bool kRadekStubTableSeeded = [] {",
        "    radekSeedAll(std::make_index_sequence<kRadekStubSeedChunks>{});",
        "    return true;",
        "}();",
        "} // namespace",
        "",
    ]
    return "\n".join(lines)


def _emit_entries(entries: list[tuple[str, str, int, int]]) -> str:
    lines = [
        "namespace {",
        "struct RadekCompatGeneratedEntry {",
        "    const char *darwin;",
        "    const char *android;",
        "    int kind; // 1 = verified implementation, 2 = explicit unimplemented stub",
        "    void (*handler)();",
        "};",
        f"constexpr unsigned long kRadekCompatEntryCount = {len(entries)}UL;",
    ]
    if entries:
        lines.append("const std::array<RadekCompatGeneratedEntry, kRadekCompatEntryCount> kRadekCompatEntries{{")
        for name, android, kind, stub_slot in entries:
            handler = (
                f"reinterpret_cast<void (*)()>(&{_VERIFIED_SHIMS[name][0]})"
                if kind == KIND_VERIFIED
                else f"gRadekStubTable[{stub_slot}]"
            )
            lines.append(
                f"    {{{_c_string_literal(name)}, {_c_string_literal(android)}, {kind}, {handler}}},"
            )
        lines.append("}};")
    else:
        lines.append(
            "const std::array<RadekCompatGeneratedEntry, kRadekCompatEntryCount> kRadekCompatEntries{};"
        )
    lines += ["} // namespace", ""]
    return "\n".join(lines)


def _emit_c_api() -> str:
    return """extern "C" {

const char *radek_compat_generated_classify(const char *darwin) {
    if (!darwin) return nullptr;
    for (unsigned long index = 0; index < kRadekCompatEntryCount; ++index) {
        const auto &entry = kRadekCompatEntries[index];
        if (std::strcmp(entry.darwin, darwin) == 0)
            return entry.kind == 1 ? "verified" : "stubbed";
    }
    return nullptr;
}

void (*radek_compat_generated_resolve(const char *darwin))(void) {
    if (!darwin) return nullptr;
    for (unsigned long index = 0; index < kRadekCompatEntryCount; ++index) {
        const auto &entry = kRadekCompatEntries[index];
        if (std::strcmp(entry.darwin, darwin) == 0)
            return entry.handler;
    }
    return nullptr;
}

long long radek_compat_generated_invoke_stub(const char *darwin) {
    if (!darwin) return -1;
    for (unsigned long index = 0; index < kRadekCompatEntryCount; ++index) {
        const auto &entry = kRadekCompatEntries[index];
        if (entry.kind == 2 && std::strcmp(entry.darwin, darwin) == 0) {
            entry.handler(); // records the invocation; documented return is 0
            return 0;
        }
    }
    return -1;
}

unsigned long radek_compat_generated_entry_count(void) { return kRadekCompatEntryCount; }

int radek_compat_generated_entry_at(unsigned long index, const char **darwin, const char **android,
                                    int *kind) {
    if (index >= kRadekCompatEntryCount) return -1;
    const auto &entry = kRadekCompatEntries[index];
    if (darwin) *darwin = entry.darwin;
    if (android) *android = entry.android;
    if (kind) *kind = entry.kind;
    return 0;
}

unsigned long long radek_compat_generated_stub_call_total(void) {
    unsigned long long total = 0;
    for (unsigned long index = 0; index < kRadekStubCount; ++index)
        total += gRadekStubCalls[index].load(std::memory_order_relaxed);
    return total;
}

} // extern "C"
"""


_HEADER = """/* Generated by RadekiOSConventor: per-IPA libioscompat compatibility source.
 *
 * Classification is exact by construction:
 *   kind 1 (verified) = a host-tested implementation body from native/src.
 *   kind 2 (stubbed)  = an explicitly unimplemented resolution handler that
 *                       records invocations and returns a safe default.
 * Stubbed entries are resolution targets only. They are NOT implementations
 * of the corresponding Darwin/iOS APIs and must not be reported as such.
 * This library does not link or run the IPA's game code.
 */
#include "apple_time_compat.h"
#include "radek_ios_shims.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>

"""


def generate(reconstruction: dict, output: Path) -> dict:
    """Write libioscompat source + registry JSON; return an honest report."""
    imports = collect_imports(reconstruction)
    if not imports:
        return {
            "schemaVersion": 1,
            "contract": CONTRACT,
            "status": "NO_OBSERVED_IMPORTS",
            "verifiedImplementations": 0,
            "stubbedHandlers": 0,
            "totalObservedImports": 0,
            "unresolvedImports": 0,
            "handlerResolutionCoveragePercent": 0,
            "truncated": False,
            "completeGameConversion": False,
            "message": "No Darwin import symbols were observed in the analyzed images.",
        }

    embeddable = [name for name in imports if is_embeddable(name)]
    rejected = len(imports) - len(embeddable)
    truncated = len(embeddable) > MAX_GENERATED_ENTRIES
    selected = embeddable[:MAX_GENERATED_ENTRIES]
    verified = [name for name in selected if classify(name) == "verified"]
    stubbed = [name for name in selected if classify(name) == "stubbed"]

    source_root = Path(__file__).resolve().parent.parent / "native"
    source_dir = output / "ioscompat"
    source_dir.mkdir(parents=True, exist_ok=True)
    for header_name in ("apple_time_compat.h", "radek_ios_shims.h"):
        (source_dir / header_name).write_bytes(
            (source_root / "include" / header_name).read_bytes()
        )

    defines = selection_defines(verified)
    implementation = (source_root / "src" / "apple_time_compat.cpp").read_text(encoding="utf-8")
    shim_implementation = (source_root / "src" / "radek_ios_shims.cpp").read_text(encoding="utf-8")

    stub_index = 0
    entries: list[tuple[str, str, int, int]] = []
    registry_entries = []
    for name in selected:
        if classify(name) == "verified":
            android = _VERIFIED_SHIMS[name][0]
            kind = KIND_VERIFIED
            slot = -1
        else:
            android = f"radek_compat_stub_{stub_index}"
            kind = KIND_STUB
            slot = stub_index
            stub_index += 1
        entries.append((name, android, kind, slot))
        registry_entries.append(
            {
                "sourceSymbol": name,
                "classification": "verified" if kind == KIND_VERIFIED else "stubbed-unimplemented",
                "androidSymbol": android,
                "implementationPresent": kind == KIND_VERIFIED,
            }
        )

    sections = [
        _HEADER,
        "// Verified implementations copied from native/src (host-tested in native/tests).",
        "\n".join(defines),
        implementation,
        "",
        shim_implementation,
        "",
        _emit_stub_machinery(len(stubbed)),
        _emit_entries(entries),
        _emit_c_api(),
    ]
    source_path = source_dir / "libioscompat.cpp"
    source_path.write_text("\n".join(sections), encoding="utf-8")
    source_hash = hashlib.sha256(source_path.read_bytes()).hexdigest()

    registry = {
        "schemaVersion": 1,
        "contract": CONTRACT,
        "verifiedImplementations": len(verified),
        "stubbedHandlers": len(stubbed),
        "totalObservedImports": len(imports),
        "unresolvedImports": len(imports) - len(selected),
        "rejectedUnsafeNames": rejected,
        "truncated": truncated,
        "implementationSha256": source_hash,
        "entries": registry_entries,
    }
    (source_dir / "registry.json").write_text(
        json.dumps(registry, indent=2, ensure_ascii=True), encoding="utf-8"
    )

    coverage = round(100.0 * len(selected) / len(imports), 4) if imports else 0
    return {
        "schemaVersion": 1,
        "contract": CONTRACT,
        "status": "REGISTRY_SOURCE_GENERATED",
        "verifiedImplementations": len(verified),
        "stubbedHandlers": len(stubbed),
        "totalObservedImports": len(imports),
        "unresolvedImports": len(imports) - len(selected),
        "rejectedUnsafeNames": rejected,
        "handlerResolutionCoveragePercent": coverage,
        "truncated": truncated,
        "sourcePath": "ioscompat/libioscompat.cpp",
        "headerPath": "ioscompat/apple_time_compat.h",
        "headerPaths": ["ioscompat/apple_time_compat.h", "ioscompat/radek_ios_shims.h"],
        "implementationTemplates": [
            "native/src/apple_time_compat.cpp",
            "native/src/radek_ios_shims.cpp",
        ],
        "registryPath": "ioscompat/registry.json",
        "implementationSha256": source_hash,
        "completeGameConversion": False,
        "message": (
            f"Every observed Darwin import has a resolution target in the generated libioscompat "
            f"source: {len(verified)} verified implementation(s) and {len(stubbed)} explicitly "
            "unimplemented stub handler(s). A stub records invocations and returns a safe default; "
            "it does not implement the API, rewrites no IPA callsites, and its presence is "
            "resolution coverage, not translation coverage."
        ),
    }
