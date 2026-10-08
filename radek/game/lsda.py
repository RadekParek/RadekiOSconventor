"""SjLj language-specific data area (LSDA) parser.

Every C++ function with cleanups or handlers stores one LSDA address (a
`GCC_except_tableN` label) into its SjLj context. The personality
(`__gxx_personality_sj`) reads the LSDA with the *call-site index* the
function recorded before the throwing call, so the call-site table is a flat
array of `(landing-pad index, action offset)` uleb128 pairs -- it carries no
PC ranges and no absolute addresses. Only two fields can hold absolute
pointers: an absolute-encoded LPStart, and absolute-encoded type-table
entries (typeinfo pointers for typed catches).

Reference: libstdc++ `eh_personality.cc` (`_GLIBCXX_SJLJ_EXCEPTIONS` path)
and libgcc `unwind-sjlj.c` (`SjLj_Function_Context`).
"""

from __future__ import annotations

from dataclasses import dataclass, field

from . import macho

OMIT = 0xFF
ABSPTR = 0x00
ULEB128 = 0x01
UDATA4 = 0x03


def _uleb(data: bytes, pos: int) -> tuple[int, int]:
    result = 0
    shift = 0
    while True:
        if pos >= len(data):
            raise macho.MachOError("truncated uleb128 in LSDA")
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, pos
        shift += 7
        if shift > 70:
            raise macho.MachOError("uleb128 overflow in LSDA")


def _sleb(data: bytes, pos: int) -> tuple[int, int]:
    result = 0
    shift = 0
    while True:
        if pos >= len(data):
            raise macho.MachOError("truncated sleb128 in LSDA")
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            if byte & 0x40:
                result -= 1 << shift
            return result, pos
        if shift > 70:
            raise macho.MachOError("sleb128 overflow in LSDA")


@dataclass
class LSDA:
    address: int
    end: int  # first byte past the action table
    callsites: list = field(default_factory=list)  # (lp_index, action_offset)
    actions: list = field(default_factory=list)  # (filter, disp, record_addr)
    action_next: dict = field(default_factory=dict)  # record_addr -> next record
    lpstart_encoding: int = OMIT
    ttype_encoding: int = OMIT
    ttype_base: int | None = None  # TType address when present
    action_base: int = 0  # first byte of the action table
    type_entries: dict = field(default_factory=dict)  # addr -> typeinfo ptr
    lpstart_addr: int | None = None  # address of absolute LPStart, if any
    byte_ranges: list = field(default_factory=list)  # (lo, hi): never pointers


def parse_lsda(image: macho.Image, address: int, limit: int) -> LSDA:
    """Parse one LSDA; `limit` bounds the read (next label or section end)."""
    blob = image.read(address, limit - address)
    lsda = LSDA(address=address, end=address)
    pos = 0
    lsda.lpstart_encoding = blob[pos]
    pos += 1
    if lsda.lpstart_encoding != OMIT:
        if lsda.lpstart_encoding != ABSPTR:
            raise macho.MachOError(
                f"LSDA@{address:#x}: unsupported LPStart encoding "
                f"{lsda.lpstart_encoding:#x}")
        lsda.lpstart_addr = address + pos
        pos += 4
    lsda.ttype_encoding = blob[pos]
    pos += 1
    if lsda.ttype_encoding != OMIT and lsda.ttype_encoding != ABSPTR:
        raise macho.MachOError(
            f"LSDA@{address:#x}: unsupported TType encoding {lsda.ttype_encoding:#x}")
    if lsda.ttype_encoding != OMIT:
        offset, pos = _uleb(blob, pos)
        lsda.ttype_base = address + pos + offset
    cs_encoding = blob[pos]
    pos += 1
    if cs_encoding not in (UDATA4, ULEB128):
        raise macho.MachOError(
            f"LSDA@{address:#x}: unexpected call-site encoding {cs_encoding:#x}")
    cs_length, pos = _uleb(blob, pos)
    cs_start = pos
    cs_end = pos + cs_length
    if cs_end > len(blob):
        raise macho.MachOError(f"LSDA@{address:#x}: call-site table overruns limit")
    # SjLj call-site table: flat (lp index, action offset) uleb128 pairs.
    # The reader ignores cs_encoding and always uses uleb128.
    while pos < cs_end:
        lp_index, pos = _uleb(blob, pos)
        action, pos = _uleb(blob, pos)
        if lp_index > 255:
            raise macho.MachOError(
                f"LSDA@{address:#x}: landing-pad index {lp_index} out of range")
        lsda.callsites.append((lp_index, action))
    lsda.byte_ranges.append((address + cs_start, address + cs_end))
    # Action table: chains of (filter, displacement) sleb128 pairs. Each
    # nonzero action offset is 1-based from the action-table start; a nonzero
    # displacement continues the chain relative to the displacement field.
    action_base = address + pos
    lsda.action_base = action_base
    action_end = action_base
    seen_chains: set = set()
    for _, action in lsda.callsites:
        if not action or action in seen_chains:
            continue
        seen_chains.add(action)
        cursor = action_base + action - 1 - address
        while True:
            if cursor < 0 or cursor >= len(blob):
                raise macho.MachOError(
                    f"LSDA@{address:#x}: action chain runs out of bounds")
            record_addr = address + cursor
            filt, after_filt = _sleb(blob, cursor)
            disp, after_disp = _sleb(blob, after_filt)
            lsda.actions.append((filt, disp, record_addr))
            action_end = max(action_end, address + after_disp)
            if disp == 0:
                lsda.action_next[record_addr] = 0
                break
            next_record = address + after_filt + disp
            lsda.action_next[record_addr] = next_record
            cursor = after_filt + disp
    lsda.byte_ranges.append((action_base, action_end))
    # Type table: absolute typeinfo pointers in the 4-aligned words between
    # the action-table end and TType. Handler filters index single entries
    # and exception-spec filters index zero-terminated lists, both 1-based
    # below TType. Every word in the span must be a typeinfo symbol or zero
    # (catch-all); anything else fails closed.
    if lsda.ttype_base is not None:
        slot = (action_end + 3) // 4 * 4
        while slot + 4 <= lsda.ttype_base:
            ptr = image.read_u32(slot)
            if ptr != 0:
                syms = image.defined_symbols_at(ptr)
                if not syms or not syms[0].name.startswith("__ZTI"):
                    raise macho.MachOError(
                        f"LSDA@{address:#x}: type-table slot {slot:#x} holds "
                        f"non-typeinfo {ptr:#x}")
                lsda.type_entries[slot] = ptr
            slot += 4
    lsda.end = action_end
    return lsda


def _points_into_image(image: macho.Image, value: int) -> bool:
    return any(
        segment.vmaddr <= value < segment.vmaddr + segment.vmsize
        for segment in image.segments
        if segment.name in ("__TEXT", "__DATA")
    )


def parse_all(image: macho.Image) -> tuple[dict, list, list]:
    """Parse every LSDA in `__gcc_except_tab`.

    Returns (lsda_by_address, inner_labels, problems). Labels are visited in
    order; a label inside a previous LSDA's extent is an inner label, not a
    new table. Anything unparseable is reported, never skipped.
    """
    section = image.section_named("__DATA", "__gcc_except_tab")
    labels = sorted(
        symbol.value for symbol in image.symbols
        if not symbol.is_stab and symbol.name.startswith("GCC_except_table")
        and section.address <= symbol.value < section.address + section.size
    )
    parsed: dict = {}
    inner: list = []
    problems: list = []
    padding: list = []
    cursor = section.address
    for index, label in enumerate(labels):
        if label < cursor:
            inner.append(label)
            continue
        bound = labels[index + 1] if index + 1 < len(labels) \
            else section.address + section.size
        # Labels can sit on alignment padding just before the LSDA the
        # function actually stores (up to 4 zero bytes); skip it.
        start = label
        while start < bound and start - label < 4:
            first = image.read(start, 1)[0]
            if first != 0:
                break
            start += 1
        if start != label:
            padding.append((label, start))
        try:
            table = parse_lsda(image, start, bound)
        except macho.MachOError as error:
            problems.append((label, str(error)))
            continue
        parsed[start] = table
        cursor = max(cursor, table.end)
    # Full coverage accounting: every section byte is either claimed by a
    # table (header, call-site/action bytes, type word, LPStart) or zero
    # padding. Anything else fails closed.
    section_end = section.address + section.size
    claimed = bytearray(section.size)
    for start, table in parsed.items():
        header_end = table.byte_ranges[0][0] if table.byte_ranges else start + 5
        for offset in range(start, min(header_end, section_end)):
            claimed[offset - section.address] = 1
        for low, high in table.byte_ranges:
            for offset in range(max(low, section.address), min(high, section_end)):
                claimed[offset - section.address] = 1
        for slot in table.type_entries:
            for offset in range(slot, slot + 4):
                if section.address <= offset < section_end:
                    claimed[offset - section.address] = 1
        if table.lpstart_addr is not None:
            for offset in range(table.lpstart_addr, table.lpstart_addr + 4):
                claimed[offset - section.address] = 1
    blob = image.read(section.address, section.size)
    run = None
    for index, byte in enumerate(blob):
        if not claimed[index] and byte != 0:
            if run is None:
                run = section.address + index
        elif run is not None:
            problems.append((run, f"non-zero unclaimed bytes to {section.address + index:#x}"))
            run = None
    if run is not None:
        problems.append((run, "non-zero unclaimed bytes to section end"))
    _ = padding
    return parsed, inner, problems
