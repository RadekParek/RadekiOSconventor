"""Runtime build generator: lifted game + data tables for the native runtime.

Reads the IPA, lifts every function (same lifter the difftest proved), and
emits the C sources + blobs the ``radek/game/rt`` runtime links against:

* ``game_all.c`` -- all lifted functions in one TU (decls + bodies).
* ``rt_gen.c``   -- DT_FUNCS/DT_ADDRS/DT_STUBS/CALLSITE_MAP tables,
  shim forwarders, mod-init table, region/relocation/binding tables.
* ``rt_gen.h``   -- shared declarations for the runtime.
* ``rt_mem.bin`` -- concatenated non-zerofill section bytes.
* ``rt_report.json`` -- stats for tests.
"""

from __future__ import annotations

import hashlib
import io
import json
import os
import re
import zipfile

from . import disasm, lift, lsda, macho, objc_meta
from .lift import sanitize


TRANSLATION_ENTRY_POINT_SYMBOL = "Java_dev_radek_gameruntime_GameBootActivity_runTranslatedGame"


def _find_exec(zf: zipfile.ZipFile) -> str:
    """Find the bundle executable from Info.plist, not from a hard-coded name."""
    names = set(zf.namelist())
    infos = sorted(
        name for name in names
        if name.startswith("Payload/") and name.endswith(".app/Info.plist")
    )
    for info_name in infos:
        try:
            import plistlib

            info = plistlib.loads(zf.read(info_name))
            executable = info.get("CFBundleExecutable")
        except (KeyError, OSError, ValueError, plistlib.InvalidFileException):
            continue
        if not isinstance(executable, str) or not executable or "/" in executable:
            continue
        candidate = info_name.rsplit("/", 1)[0] + "/" + executable
        if candidate in names:
            return candidate
    # Keep the original Angry Birds path as a compatibility fallback for old
    # fixtures that deliberately omit or use an unreadable plist.
    for name in sorted(names):
        if name.startswith("Payload/") and name.endswith(".app/AngryBirds"):
            return name
    raise ValueError("bundle executable could not be found from Info.plist")


def load_ipa(ipa_path: str):
    with open(ipa_path, "rb") as f:
        zf = zipfile.ZipFile(io.BytesIO(f.read()))
    raw = zf.read(_find_exec(zf))
    img = macho.parse(raw)
    funcs = disasm.disassemble_all(img)
    ctx = lift.build_context(img, funcs)
    return img, funcs, ctx


def _shim_name(symbol: str) -> str:
    return "shim_" + sanitize(symbol.lstrip("_"))


def _collect_shims(ctx, out) -> list[str]:
    allc = "\n".join(out.values())
    calls = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", allc))
    defined = set(ctx.cname.values())
    helpers = {
        "if", "while", "for", "switch", "return", "goto", "sizeof",
        "rd32", "wr32", "rd16", "wr16", "rd8", "wr8", "rd64", "wr64",
        "sget", "sset", "u2f", "f2u", "u2d", "d2u", "cc_pass",
        "fl_nz", "fl_add", "fl_sub", "fl_adc", "fl_sbc", "sqrtf",
        "sqrt", "isnan", "rint", "vcvt_s32_f", "vcvt_s32_d",
        "vcvt_u32_f", "vcvt_u32_d", "vfp_nzcv_f", "vfp_nzcv_d",
        "memcpy", "tdispatch",
    }
    shims = sorted(n for n in calls - defined - helpers if n.startswith("shim_"))
    for sym in ctx.import_of_stub.values():
        nm = _shim_name(sym)
        if nm not in shims:
            shims.append(nm)
    return sorted(shims)


def _translated_text_coverage(funcs, translated_addrs, text_address: int, text_size: int) -> dict:
    """Count unique bytes in successfully emitted instructions inside ``__text``.

    Function symbols can overlap or contain duplicate instruction ranges. Sum of
    function sizes therefore is not a code-byte measure; merge the actual
    emitted instruction intervals and clip them to the executable text section.
    """
    text_end = text_address + text_size
    intervals: list[tuple[int, int]] = []
    summed_function_bytes = 0
    summed_text_instruction_bytes = 0
    instruction_count = 0
    for address in translated_addrs:
        for instruction_address, _mnemonic, _operands, encoded in funcs[address].instructions:
            instruction_size = len(encoded)
            if instruction_size <= 0:
                continue
            instruction_count += 1
            summed_function_bytes += instruction_size
            start = max(int(instruction_address), text_address)
            end = min(int(instruction_address) + instruction_size, text_end)
            if start < end:
                summed_text_instruction_bytes += end - start
                intervals.append((start, end))

    intervals.sort()
    unique_bytes = 0
    merged_start = merged_end = None
    for start, end in intervals:
        if merged_start is None:
            merged_start, merged_end = start, end
        elif start <= merged_end:
            merged_end = max(merged_end, end)
        else:
            unique_bytes += merged_end - merged_start
            merged_start, merged_end = start, end
    if merged_start is not None:
        unique_bytes += merged_end - merged_start

    return {
        "uniqueTextBytes": unique_bytes,
        "summedFunctionInstructionBytes": summed_function_bytes,
        "summedTextInstructionBytes": summed_text_instruction_bytes,
        "overlappingTextInstructionBytes": summed_text_instruction_bytes - unique_bytes,
        "translatedInstructionCount": instruction_count,
    }


def _callsites(funcs, addrs) -> set[int]:
    sites = set()
    for a in addrs:
        for (x, m, _o, b) in funcs[a].instructions:
            # The lifted backend also uses VRET_BIT for register/PC-relative
            # dispatch continuations (not only BL/BLX). Mark every decoded
            # instruction address so those continuations return to the C
            # caller without weakening the range check to arbitrary memory.
            sites.add(x)
            w = int.from_bytes(b, "little")
            if ((w >> 25) & 7) == 5 and (w >> 24) & 1:
                sites.add(x)
            elif m == "blx":
                sites.add(x)
    return sites


def generate(ipa_path: str, out_dir: str) -> dict:
    img, funcs, ctx = load_ipa(ipa_path)
    objc = objc_meta.parse(img)
    lsda_tables, lsda_inner, lsda_problems = lsda.parse_all(img)
    out, failures = lift.lift_all(ctx)
    assert not failures, failures[:5]
    addrs = sorted(out)
    shims = _collect_shims(ctx, out)
    objc_methods = [
        (owner, selector, imp)
        for owner, selector, _types, imp in objc.methods
        if not owner.startswith("proto:") and imp and imp in ctx.cname
    ]
    os.makedirs(out_dir, exist_ok=True)

    # ---- game_all.c ----
    with open(os.path.join(out_dir, "game_all.c"), "w") as f:
        f.write('#include "cpu.h"\n')
        for a in addrs:
            f.write(f"void {ctx.cname[a]}(CPU *cpu);\n")
        for n in shims:
            f.write(f"void {n}(CPU *cpu);\n")
        for a in addrs:
            f.write(out[a])
            f.write("\n")

    # ---- rt_mem.bin + region table ----
    regions = []  # (addr, size, blob_off, is_zero)
    blob = bytearray()
    for s in img.sections:
        if s.size == 0:
            continue
        if s.type == macho.S_ZEROFILL:
            regions.append((s.address, s.size, 0, 1))
        else:
            regions.append((s.address, s.size, len(blob), 0))
            blob += img.data[s.file_offset:s.file_offset + s.size]
    with open(os.path.join(out_dir, "rt_mem.bin"), "wb") as f:
        f.write(bytes(blob))

    # ---- ext-reloc table (addr -> symbol) ----
    extrel = []
    for r in img.external_relocations:
        sym = img.symbols[r.symbol_index]
        name = sym.name if hasattr(sym, "name") else str(sym)
        extrel.append((r.address, name))

    # ---- non-lazy pointer syms (slot addr -> symbol) ----
    nl_sec = img.section_named("__DATA", "__nl_symbol_ptr")
    nl_syms = []
    if nl_sec is not None:
        base = nl_sec.address
        idx0 = nl_sec.reserved1
        nslots = nl_sec.size // 4
        for k in range(nslots):
            si = img.indirect_symbols[idx0 + k]
            if si is None or si == 0x80000000 or (isinstance(si, int) and si < 0):
                continue
            try:
                sym = img.symbols[si]
            except Exception:
                continue
            name = sym.name if hasattr(sym, "name") else str(sym)
            nl_syms.append((base + 4 * k, name))

    # ---- la_ptr syms (slot addr -> symbol), for table completeness ----
    la_sec = img.section_named("__DATA", "__la_symbol_ptr")
    la_syms = []
    if la_sec is not None:
        base = la_sec.address
        idx0 = la_sec.reserved1
        nslots = la_sec.size // 4
        for k in range(nslots):
            si = img.indirect_symbols[idx0 + k]
            if si is None or si == 0x80000000 or (isinstance(si, int) and si < 0):
                continue
            try:
                sym = img.symbols[si]
            except Exception:
                continue
            name = sym.name if hasattr(sym, "name") else str(sym)
            la_syms.append((base + 4 * k, name))

    # ---- mod-init addrs ----
    mod_sec = img.section_named("__DATA", "__mod_init_func")
    modinits = []
    if mod_sec is not None:
        for k in range(mod_sec.size // 4):
            modinits.append(img.read_u32(mod_sec.address + 4 * k))

    # ---- main addr ----
    main_addr = None
    for s in img.symbols:
        if getattr(s, "name", "") == "_main" and getattr(s, "n_sect", 0) != 0:
            main_addr = s.value
            break
    assert main_addr is not None, "no defined _main"

    # ---- stub map (stub addr -> symbol) ----
    stub_syms = sorted(macho.stub_map(img))

    # ---- rt_gen.c ----
    sites = _callsites(funcs, addrs)
    with open(os.path.join(out_dir, "rt_gen.c"), "w") as f:
        f.write('#include "rt_gen.h"\n')
        f.write('#include "cpu.h"\n')
        for a in addrs:
            f.write(f"void {ctx.cname[a]}(CPU *cpu);\n")
        f.write("void (*DT_FUNCS[])(CPU *cpu) = {\n")
        for a in addrs:
            f.write(f"    {ctx.cname[a]},\n")
        f.write("};\nunsigned DT_NFUNCS = sizeof(DT_FUNCS)/sizeof(DT_FUNCS[0]);\n")
        f.write("const uint32_t DT_ADDRS[] = {\n")
        for a in addrs:
            f.write(f"    0x{a:x}u,\n")
        f.write("};\n")
        for n in shims:
            f.write(f"void {n}(CPU *cpu);\n")
        f.write("void (*DT_SHIM_FUNCS[])(CPU *cpu) = {\n")
        for n in shims:
            f.write(f"    {n},\n")
        f.write("};\n")
        f.write("const DT_STUB DT_STUBS[] = {\n")
        for sa, _slot, sym in stub_syms:
            nm = _shim_name(sym)
            f.write(f"    {{0x{sa:x}u, {shims.index(nm)}u}},\n")
        f.write("};\nunsigned DT_NSTUBS = sizeof(DT_STUBS)/sizeof(DT_STUBS[0]);\n")
        if sites:
            lo = min(sites) & ~3
            hi = (max(sites) + 7) & ~3
            f.write(f"unsigned DT_CALL_LO = 0x{lo:x}u;\n")
            f.write(f"unsigned DT_CALL_HI = 0x{hi:x}u;\n")
            f.write("const unsigned char CALLSITE_MAP[] = {\n")
            row = []
            for s in range(lo, hi, 4):
                row.append("1" if s in sites else "0")
                if len(row) == 32:
                    f.write("    " + ",".join(row) + ",\n")
                    row = []
            if row:
                f.write("    " + ",".join(row) + ",\n")
            f.write("};\n")
        else:
            # Keep the generated runtime linkable for a valid no-call image.
            f.write("unsigned DT_CALL_LO = 0u;\n")
            f.write("unsigned DT_CALL_HI = 0u;\n")
            f.write("const unsigned char CALLSITE_MAP[] = {0};\n")
        f.write("static const char *SHIM_SYMS[] = {\n")
        for n in shims:
            sym = n[len("shim_"):]
            f.write(f"    \"{sym}\",\n")
        f.write("};\n")
        # shim forwarders: every shim calls into the runtime by default.
        for i, n in enumerate(shims):
            f.write(f"void {n}(CPU *cpu) {{ rt_shim(cpu, {i}); }}\n")
        f.write("const char *rt_shim_symbol(unsigned i) {\n")
        f.write(f"    if (i >= {len(shims)}u) return \"?\";\n")
        f.write("    return SHIM_SYMS[i];\n}\n")
        f.write(f"unsigned RT_NSHIMS = {len(shims)};\n")
        # regions
        f.write("const RT_REGION RT_REGIONS[] = {\n")
        for (a, ln, off, zero) in regions:
            f.write(f"    {{0x{a:x}u, 0x{ln:x}u, 0x{off:x}u, {zero}}},\n")
        f.write("};\n")
        f.write(f"unsigned RT_NREGIONS = {len(regions)};\n")
        # ext relocs
        f.write("const RT_BIND RT_EXTREL[] = {\n")
        for (a, name) in extrel:
            esc = name.replace("\\", "\\\\").replace('"', '\\"')
            f.write(f'    {{0x{a:x}u, "{esc}"}},\n')
        f.write("};\n")
        f.write(f"unsigned RT_NEXTREL = {len(extrel)};\n")
        # nl/la syms
        f.write("const RT_BIND RT_NLSYM[] = {\n")
        for (a, name) in nl_syms:
            esc = name.replace("\\", "\\\\").replace('"', '\\"')
            f.write(f'    {{0x{a:x}u, "{esc}"}},\n')
        f.write("};\n")
        f.write(f"unsigned RT_NNLSYM = {len(nl_syms)};\n")
        f.write("const RT_BIND RT_LASYM[] = {\n")
        for (a, name) in la_syms:
            esc = name.replace("\\", "\\\\").replace('"', '\\"')
            f.write(f'    {{0x{a:x}u, "{esc}"}},\n')
        f.write("};\n")
        f.write(f"unsigned RT_NLASYM = {len(la_syms)};\n")
        # Objective-C class/method metadata used by the bounded runtime
        # dispatcher. Protocol declarations remain report-only; concrete
        # class methods are emitted only when their IMP was lifted.
        f.write("const RT_OBJC_CLASS RT_OBJC_CLASSES[] = {\n")
        if objc.classes:
            for class_addr, _meta_addr, class_name in objc.classes:
                esc = class_name.replace("\\", "\\\\").replace('"', '\\"')
                f.write(f'    {{0x{class_addr:x}u, "{esc}"}},\n')
        else:
            f.write("    {0u, \"\"},\n")
        f.write("};\n")
        f.write(f"unsigned RT_NOBJC_CLASSES = {len(objc.classes)};\n")
        f.write("const RT_OBJC_METHOD RT_OBJC_METHODS[] = {\n")
        if objc_methods:
            for owner, selector, imp in objc_methods:
                owner_esc = owner.replace("\\", "\\\\").replace('"', '\\"')
                selector_esc = selector.replace("\\", "\\\\").replace('"', '\\"')
                f.write(f'    {{"{owner_esc}", "{selector_esc}", 0x{imp:x}u}},\n')
        else:
            f.write("    {\"\", \"\", 0u},\n")
        f.write("};\n")
        f.write(f"unsigned RT_NOBJC_METHODS = {len(objc_methods)};\n")
        f.write("const RT_LSDA_TABLE RT_LSDA_TABLES[] = {\n")
        site_first = 0
        if lsda_tables:
            for table_addr, table in sorted(lsda_tables.items()):
                f.write(
                    f"    {{0x{table_addr:x}u, 0x{table.end:x}u, {site_first}u, "
                    f"0x{table.action_base:x}u, "
                    f"0x{(table.ttype_base or 0):x}u, {len(table.callsites)}u, "
                    f"{len(table.actions)}u, {len(table.type_entries)}u}},\n"
                )
                site_first += len(table.callsites)
        else:
            f.write("    {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},\n")
        f.write("};\n")
        f.write(f"unsigned RT_NLSDA_TABLES = {len(lsda_tables)};\n")
        f.write("const RT_LSDA_SITE RT_LSDA_SITES[] = {\n")
        site_index = 0
        if lsda_tables:
            for table_addr, table in sorted(lsda_tables.items()):
                for landing, action in table.callsites:
                    f.write(
                        f"    {{0x{table_addr:x}u, {site_index}u, "
                        f"{landing}u, {action}u}},\n"
                    )
                    site_index += 1
        else:
            f.write("    {0u, 0u, 0u, 0u},\n")
        f.write("};\n")
        f.write(f"unsigned RT_NLSDA_SITES = {site_index};\n")
        f.write("const RT_LSDA_ACTION RT_LSDA_ACTIONS[] = {\n")
        action_index = 0
        if lsda_tables:
            for table_addr, table in sorted(lsda_tables.items()):
                for filt, displacement, record_addr in table.actions:
                    f.write(
                        f"    {{0x{table_addr:x}u, 0x{record_addr:x}u, "
                        f"0x{table.action_next.get(record_addr, 0):x}u, "
                        f"{filt}, {displacement}}},\n"
                    )
                    action_index += 1
        else:
            f.write("    {0u, 0u, 0u, 0, 0},\n")
        f.write("};\n")
        f.write(f"unsigned RT_NLSDA_ACTIONS = {action_index};\n")
        f.write("const RT_LSDA_TYPE RT_LSDA_TYPES[] = {\n")
        wrote_lsda_type = False
        for table_addr, table in sorted(lsda_tables.items()):
            for slot, typeinfo in sorted(table.type_entries.items()):
                f.write(f"    {{0x{table_addr:x}u, 0x{slot:x}u, 0x{typeinfo:x}u}},\n")
                wrote_lsda_type = True
        if not wrote_lsda_type:
            f.write("    {0u, 0u, 0u},\n")
        f.write("};\n")
        f.write(
            f"unsigned RT_NLSDA_TYPES = "
            f"{sum(len(table.type_entries) for table in lsda_tables.values())};\n"
        )
        # mod inits
        f.write("void (*RT_MODINITS[])(CPU *cpu) = {\n")
        for a in modinits:
            assert a in ctx.cname, f"modinit {a:#x} not lifted"
            f.write(f"    {ctx.cname[a]},\n")
        f.write("};\n")
        f.write("const uint32_t RT_MODINIT_ADDRS[] = {\n")
        for a in modinits:
            f.write(f"    0x{a:x}u,\n")
        f.write("};\n")
        f.write(f"unsigned RT_NMODINITS = {len(modinits)};\n")
        # main
        assert main_addr in ctx.cname, "main not lifted"
        f.write(f"void (*RT_MAIN)(CPU *cpu) = {ctx.cname[main_addr]};\n")
        f.write(f"uint32_t RT_MAIN_ADDR = 0x{main_addr:x}u;\n")

    with open(os.path.join(out_dir, "rt_gen.h"), "w") as f:
        f.write("#ifndef RADEK_GAME_RT_GEN_H\n")
        f.write("#define RADEK_GAME_RT_GEN_H\n")
        f.write('#include <stdint.h>\n')
        f.write('typedef struct CPU CPU;\n')
        f.write("typedef struct { uint32_t addr; uint32_t len; uint32_t blob; int zero; } RT_REGION;\n")
        f.write("typedef struct { uint32_t addr; const char *sym; } RT_BIND;\n")
        f.write("typedef struct { uint32_t addr; const char *name; } RT_OBJC_CLASS;\n")
        f.write("typedef struct { const char *owner; const char *selector; uint32_t imp; } RT_OBJC_METHOD;\n")
        f.write("typedef struct { uint32_t addr; uint32_t end; uint32_t site_first; uint32_t action_base; uint32_t ttype_base; unsigned callsites; unsigned actions; unsigned types; } RT_LSDA_TABLE;\n")
        f.write("typedef struct { uint32_t table; unsigned index; unsigned landing; unsigned action; } RT_LSDA_SITE;\n")
        f.write("typedef struct { uint32_t table; uint32_t record; uint32_t next; int filter; int displacement; } RT_LSDA_ACTION;\n")
        f.write("typedef struct { uint32_t table; uint32_t slot; uint32_t typeinfo; } RT_LSDA_TYPE;\n")
        f.write("extern const RT_OBJC_CLASS RT_OBJC_CLASSES[];\nextern unsigned RT_NOBJC_CLASSES;\n")
        f.write("extern const RT_LSDA_TABLE RT_LSDA_TABLES[];\nextern unsigned RT_NLSDA_TABLES;\n")
        f.write("extern const RT_LSDA_SITE RT_LSDA_SITES[];\nextern unsigned RT_NLSDA_SITES;\n")
        f.write("extern const RT_LSDA_ACTION RT_LSDA_ACTIONS[];\nextern unsigned RT_NLSDA_ACTIONS;\n")
        f.write("extern const RT_LSDA_TYPE RT_LSDA_TYPES[];\nextern unsigned RT_NLSDA_TYPES;\n")
        f.write("extern const RT_OBJC_METHOD RT_OBJC_METHODS[];\nextern unsigned RT_NOBJC_METHODS;\n")
        f.write("extern void (*DT_FUNCS[])(CPU *cpu);\nextern unsigned DT_NFUNCS;\n")
        f.write("extern const uint32_t DT_ADDRS[];\n")
        f.write("extern void (*DT_SHIM_FUNCS[])(CPU *cpu);\n")
        f.write("typedef struct { uint32_t addr; unsigned shim; } DT_STUB;\n")
        f.write("extern const DT_STUB DT_STUBS[];\nextern unsigned DT_NSTUBS;\n")
        f.write("extern unsigned DT_CALL_LO;\nextern unsigned DT_CALL_HI;\n")
        f.write("extern const unsigned char CALLSITE_MAP[];\n")
        f.write("extern unsigned RT_NSHIMS;\n")
        f.write("const char *rt_shim_symbol(unsigned i);\n")
        f.write("void rt_shim(CPU *cpu, unsigned i);\n")
        f.write("extern const RT_REGION RT_REGIONS[];\nextern unsigned RT_NREGIONS;\n")
        f.write("extern const RT_BIND RT_EXTREL[];\nextern unsigned RT_NEXTREL;\n")
        f.write("extern const RT_BIND RT_NLSYM[];\nextern unsigned RT_NNLSYM;\n")
        f.write("extern const RT_BIND RT_LASYM[];\nextern unsigned RT_NLASYM;\n")
        f.write("extern void (*RT_MODINITS[])(CPU *cpu);\n")
        f.write("extern const uint32_t RT_MODINIT_ADDRS[];\n")
        f.write("extern unsigned RT_NMODINITS;\n")
        f.write("extern void (*RT_MAIN)(CPU *cpu);\nextern uint32_t RT_MAIN_ADDR;\n")
        f.write("#endif\n")

    text_section = img.section_named("__TEXT", "__text")
    executable_text_bytes = int(getattr(text_section, "size", 0) or 0)
    text_address = int(getattr(text_section, "address", 0) or 0)
    translated_coverage = _translated_text_coverage(
        funcs, addrs, text_address, executable_text_bytes
    )
    translated_function_bytes = translated_coverage["uniqueTextBytes"]
    function_symbols = [ctx.cname[address] for address in addrs]
    report = {
        "status": "GENERATED",
        "sourceExecutableSha256": hashlib.sha256(img.data).hexdigest(),
        "translationEntryPointSymbol": TRANSLATION_ENTRY_POINT_SYMBOL,
        "functions": len(addrs),
        "functionFailures": 0,
        "shims": len(shims),
        "regions": len(regions),
        "blob_bytes": len(blob),
        "extrel": len(extrel),
        "nlsym": len(nl_syms),
        "lasym": len(la_syms),
        "modinits": len(modinits),
        "objcClasses": len(objc.classes),
        "objcMethods": len(objc_methods),
        "objcProtocols": len(objc.protocols),
        "objcBoundSymbols": len(objc.bound_symbols),
        "objcProblems": len(objc.problems),
        "lsdaTables": len(lsda_tables),
        "lsdaInnerLabels": len(lsda_inner),
        "lsdaSites": sum(len(table.callsites) for table in lsda_tables.values()),
        "lsdaActions": sum(len(table.actions) for table in lsda_tables.values()),
        "lsdaTypeEntries": sum(len(table.type_entries) for table in lsda_tables.values()),
        "lsdaProblems": len(lsda_problems),
        "main_addr": main_addr,
        "executableTextBytes": executable_text_bytes,
        "translatedFunctionBytes": translated_function_bytes,
        "translatedUniqueTextBytes": translated_function_bytes,
        "summedFunctionInstructionBytes": translated_coverage["summedFunctionInstructionBytes"],
        "summedTextInstructionBytes": translated_coverage["summedTextInstructionBytes"],
        "overlappingTextInstructionBytes": translated_coverage["overlappingTextInstructionBytes"],
        "translatedInstructionCount": translated_coverage["translatedInstructionCount"],
        "translatedFunctionSymbols": function_symbols,
        "translatedTextPercent": round(
            min(100.0, 100.0 * translated_function_bytes / executable_text_bytes), 6
        ) if executable_text_bytes else 0.0,
        "translationBackend": "radek.game.lift -> portable C ARM32 state runtime",
        "linkedIntoGame": False,
        "apkProduced": False,
    }
    with open(os.path.join(out_dir, "rt_report.json"), "w") as f:
        json.dump(report, f, indent=2)
    return report


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("ipa")
    ap.add_argument("out_dir")
    ns = ap.parse_args(argv)
    rep = generate(ns.ipa, ns.out_dir)
    print(json.dumps(rep, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
