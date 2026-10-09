"""ARM disassembly with literal and cross-reference resolution.

Recursive-descent disassembly over the game's `__text`, anchored at every
known function entry (symbols, thread entry, static initializers, stub table).
For each PC-relative literal load the referenced data word is recorded, and a
bounded intra-procedural use analysis tells pointer literals (which must slide
with the image) apart from integer/float constants (which must not). Anything
undecidable is reported as ambiguous with evidence instead of guessed.

All instruction dispatch uses capstone instruction *ids*, never mnemonic
strings: capstone folds ARM condition codes into mnemonics (`beq`, `ldrle`),
so string matching would silently miss conditional branches, loads and calls.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

from . import macho

try:
    import capstone as _capstone
    from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs
    from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG
    from capstone.arm_const import ARM_CC_AL, ARM_CC_INVALID
    from capstone import arm_const as _arm_const

    _IDS = {
        name: getattr(_arm_const, name)
        for name in (
            "ARM_INS_B", "ARM_INS_BL", "ARM_INS_BLX", "ARM_INS_BX",
            "ARM_INS_LDR", "ARM_INS_LDRB", "ARM_INS_LDRH", "ARM_INS_LDRSB",
            "ARM_INS_LDRSH", "ARM_INS_LDRD", "ARM_INS_VLDR",
            "ARM_INS_STR", "ARM_INS_STRB", "ARM_INS_STRH", "ARM_INS_STRD",
            "ARM_INS_LDM", "ARM_INS_STM", "ARM_INS_POP", "ARM_INS_PUSH",
            "ARM_INS_MOV", "ARM_INS_MVN", "ARM_INS_ADR",
            "ARM_INS_ADD", "ARM_INS_ADC", "ARM_INS_SUB", "ARM_INS_SBC",
            "ARM_INS_RSB", "ARM_INS_RSC", "ARM_INS_AND", "ARM_INS_ORR",
            "ARM_INS_EOR", "ARM_INS_BIC", "ARM_INS_MUL", "ARM_INS_MLA",
            "ARM_INS_MLS", "ARM_INS_UMULL", "ARM_INS_SMULL",
            "ARM_INS_UMLAL", "ARM_INS_SMLAL", "ARM_INS_LSL", "ARM_INS_LSR",
            "ARM_INS_ASR", "ARM_INS_ROR", "ARM_INS_RRX", "ARM_INS_CLZ",
            "ARM_INS_CMP", "ARM_INS_CMN", "ARM_INS_TST", "ARM_INS_TEQ",
        )
    }
except ImportError:  # pragma: no cover - dependency is documented in docs/BUILD.md
    _capstone = None
    Cs = None
    ARM_CC_AL, ARM_CC_INVALID = 15, 0
    _IDS = {}


def _id(*names: str) -> set:
    return {_IDS[name] for name in names if name in _IDS}


_CALL = _id("ARM_INS_BL", "ARM_INS_BLX")
_BRANCH = _id("ARM_INS_B")
_LITERAL_LOADS = _id(
    "ARM_INS_LDR", "ARM_INS_LDRB", "ARM_INS_LDRH", "ARM_INS_LDRSB",
    "ARM_INS_LDRSH", "ARM_INS_LDRD", "ARM_INS_VLDR",
)
_STORES = _id("ARM_INS_STR", "ARM_INS_STRB", "ARM_INS_STRH", "ARM_INS_STRD")
_MULTI_STORE = _id("ARM_INS_STM", "ARM_INS_PUSH")
_MULTI_LOAD = _id("ARM_INS_LDM", "ARM_INS_POP")
_COMPARES = _id("ARM_INS_CMP", "ARM_INS_CMN", "ARM_INS_TST", "ARM_INS_TEQ")
_COPIES = _id("ARM_INS_MOV", "ARM_INS_MVN")
_DERIVING = _id("ARM_INS_ADD", "ARM_INS_SUB", "ARM_INS_ADR")
_VALUE_ALU = _id(
    "ARM_INS_ADD", "ARM_INS_SUB", "ARM_INS_AND", "ARM_INS_ORR", "ARM_INS_EOR",
    "ARM_INS_RSB", "ARM_INS_MUL", "ARM_INS_LSL", "ARM_INS_LSR",
    "ARM_INS_ASR", "ARM_INS_ROR",
)
_REDEFINING_FIRST = _LITERAL_LOADS | _id(
    "ARM_INS_MOV", "ARM_INS_MVN", "ARM_INS_ADR",
    "ARM_INS_ADD", "ARM_INS_ADC", "ARM_INS_SUB", "ARM_INS_SBC",
    "ARM_INS_RSB", "ARM_INS_RSC", "ARM_INS_AND", "ARM_INS_ORR",
    "ARM_INS_EOR", "ARM_INS_BIC", "ARM_INS_MUL", "ARM_INS_MLA",
    "ARM_INS_MLS", "ARM_INS_UMULL", "ARM_INS_SMULL",
    "ARM_INS_UMLAL", "ARM_INS_SMLAL", "ARM_INS_LSL", "ARM_INS_LSR",
    "ARM_INS_ASR", "ARM_INS_ROR", "ARM_INS_RRX", "ARM_INS_CLZ",
)

_CS_SINGLETON = []


def require_capstone():
    if Cs is None or _capstone is None:
        raise RuntimeError(
            "radek.game.disasm needs capstone >= 5.0.6,<6.0.0 (pip install 'capstone==5.0.7')"
        )
    version_match = re.match(r"^(\d+)\.(\d+)\.(\d+)", str(_capstone.__version__))
    version = tuple(map(int, version_match.groups())) if version_match is not None else None
    if version is None or version < (5, 0, 6) or version >= (6, 0, 0):
        installed = getattr(_capstone, "__version__", "unknown")
        raise RuntimeError(
            "radek.game.disasm needs capstone >= 5.0.6,<6.0.0 for complete ARM detail "
            f"operands; found {installed}"
        )
    if not _CS_SINGLETON:
        engine = Cs(CS_ARCH_ARM, CS_MODE_ARM)
        engine.detail = True
        _CS_SINGLETON.append(engine)
    else:
        engine = _CS_SINGLETON[0]
        engine.detail = True
    return engine


# ---------------------------------------------------------------------------
# Model


@dataclass
class LiteralUse:
    """One PC-relative literal load and what its value is used for."""

    address: int  # address of the LDR/VLDR instruction
    register: int  # destination register number (or -1 for VFP)
    literal_address: int  # address of the referenced data word
    value: int  # word stored at the literal address
    verdict: str  # POINTER | VALUE | AMBIGUOUS | OUT_OF_RANGE
    evidence: str = ""
    store_sites: list = field(default_factory=list)  # (addr, base_reg|None)


@dataclass
class CallSite:
    address: int
    target: int | None  # direct target, or None for indirect calls


@dataclass
class Function:
    address: int
    name: str
    size: int = 0
    instructions: list = field(default_factory=list)  # (addr, mnemonic, op_str, bytes)
    code_words: set = field(default_factory=set)
    literals: list = field(default_factory=list)  # LiteralUse
    calls: set = field(default_factory=set)  # direct BL targets
    call_sites: list = field(default_factory=list)  # CallSite
    branches: set = field(default_factory=set)  # direct B targets
    indirect_branches: list = field(default_factory=list)
    jump_tables: list = field(default_factory=list)  # (table_base, entry_count)
    ends_structured: bool = False  # ended by control flow, not undecodable bytes


# ---------------------------------------------------------------------------
# Small helpers

_REG_ALIASES = {
    9: ("r9", "sb"),
    10: ("r10", "sl"),
    11: ("r11", "fp"),
    12: ("r12", "ip"),
    13: ("r13", "sp"),
    14: ("r14", "lr"),
    15: ("r15", "pc"),
}

_TOKEN_REG = {
    "r0": 0, "r1": 1, "r2": 2, "r3": 3, "r4": 4, "r5": 5, "r6": 6, "r7": 7,
    "r8": 8, "sb": 9, "r9": 9, "sl": 10, "r10": 10, "fp": 11, "r11": 11,
    "ip": 12, "r12": 12, "sp": 13, "r13": 13, "lr": 14, "r14": 14,
    "pc": 15, "r15": 15,
}


def _reg_names(reg: int) -> tuple:
    if reg in _REG_ALIASES:
        return _REG_ALIASES[reg]
    return (f"r{reg}",)


def _token_reg(token: str):
    return _TOKEN_REG.get(token)


def _split_operands(operands: str) -> list:
    return (
        operands.replace("[", " [ ")
        .replace("]", " ] ")
        .replace(",", " ")
        .replace("{", " { ")
        .replace("}", " } ")
        .replace("!", " ! ")
        .split()
    )


def _is_unconditional(insn) -> bool:
    return insn.cc in (ARM_CC_AL, ARM_CC_INVALID)


def _branch_target(insn):
    """Direct branch/call target address, or None."""
    if insn.id not in _BRANCH | _CALL:
        return None
    if not insn.operands or insn.operands[0].type != ARM_OP_IMM:
        return None
    return insn.operands[0].imm


def _is_return(insn) -> bool:
    """True for unconditional function returns (any condition code)."""
    if not _is_unconditional(insn):
        return False
    if insn.id in _id("ARM_INS_BX") and insn.op_str.strip() == "lr":
        return True
    if insn.id in _id("ARM_INS_POP"):
        regs = [token for token in _split_operands(insn.op_str) if _token_reg(token) == 15]
        if regs:
            return True
    if insn.id in _id("ARM_INS_MOV") and insn.op_str.strip() == "pc, lr":
        return True
    if insn.id in _id("ARM_INS_LDR"):
        tokens = _split_operands(insn.op_str)
        if tokens and _token_reg(tokens[0]) == 15 and "[sp]" in insn.op_str.replace(" ", ""):
            return True
    return False


def _dest_reg(insn) -> int:
    if not insn.operands:
        return -1
    first = insn.operands[0]
    if first.type != ARM_OP_REG:
        return -1
    name = insn.reg_name(first.reg)
    if name in _TOKEN_REG:
        return _TOKEN_REG[name]
    return -1  # VFP / SIMD destination


def image_span(image: macho.Image) -> tuple:
    mapped = [
        (segment.vmaddr, segment.vmaddr + segment.vmsize)
        for segment in image.segments
        if segment.name in ("__TEXT", "__DATA")
    ]
    low = min(low for low, _ in mapped)
    high = max(high for _, high in mapped)
    return low, high, mapped


def _points_into_image(image: macho.Image, value: int) -> bool:
    _, _, mapped = image_span(image)
    return any(low <= value < high for low, high in mapped)


# ---------------------------------------------------------------------------
# Stubs and noreturn calls

#: Imported symbols that never return to their caller. Calls through these
#: stubs end the control-flow path: whatever bytes follow are data (landing
#: pads, literal pools), never fallthrough code.
NORETURN_IMPORTS = frozenset({
    "__Unwind_SjLj_Resume",
    "___cxa_throw",
    "___cxa_rethrow",
    "__ZSt9terminatev",
    "_abort",
    "_exit",
    "_longjmp",
    "_pthread_exit",
    "_objc_exception_throw",
    "_objc_terminate",
})


def stub_targets(image: macho.Image) -> dict:
    """Map each symbol-stub address to the imported symbol name."""
    stubs = image.section_named("__TEXT", "__symbol_stub4")
    stride = stubs.reserved2 or 12
    targets = {}
    for i in range(stubs.size // stride):
        raw = image.indirect_symbols[stubs.reserved1 + i]
        if raw & (macho.INDIRECT_SYMBOL_LOCAL | macho.INDIRECT_SYMBOL_ABS):
            continue
        targets[stubs.address + i * stride] = image.symbols[raw].name
    return targets


def noreturn_stubs(image: macho.Image) -> set:
    """Addresses of stubs that never return to their caller."""
    return {
        address
        for address, name in stub_targets(image).items()
        if name in NORETURN_IMPORTS
    }


# ---------------------------------------------------------------------------
# Entries


def function_entries(image: macho.Image) -> dict:
    """Collect every known function entry address -> name."""
    entries = {}
    text = image.section_named("__TEXT", "__text")

    def add(address: int, name: str):
        if text.address <= address < text.address + text.size and address % 4 == 0:
            entries.setdefault(address, name)

    if image.thread_entry.get("pc"):
        add(image.thread_entry["pc"], "start")
    try:
        inits = image.section_named("__DATA", "__mod_init_func")
        for i in range(inits.size // 4):
            add(image.read_u32(inits.address + i * 4), f"mod_init_{i}")
    except macho.MachOError:
        pass
    text_index = text.index
    for symbol in image.symbols:
        if symbol.is_stab or symbol.n_sect != text_index or not symbol.value:
            continue
        if not symbol.is_defined_in_section and symbol.name not in ("start", "_main"):
            continue
        if symbol.name.startswith("ltmp") or symbol.name.startswith("L"):
            continue
        add(symbol.value, symbol.name or f"sub_{symbol.value:x}")
    return entries


# ---------------------------------------------------------------------------
# Literal use analysis

_CALL_CLOBBERED = frozenset({0, 1, 2, 3, 12})

_WORD_STORES = _id("ARM_INS_STR")
_WORD_LOADS = _id("ARM_INS_LDR")
_FRAME_REGS = frozenset({7, 11, 13})  # r7, fp, sp


def _stack_slot(operands: str):
    """(base, offset) for a `[base, #imm]` stack access, else None.

    Only word-sized accesses with a constant offset are keyable; anything
    else (register offsets, writeback-then-use) is conservatively treated
    as an unkeyable access by the caller.
    """
    if "[" not in operands or "]" not in operands:
        return None
    tokens = _split_operands(operands)
    try:
        base_token = tokens[tokens.index("[") + 1]
    except IndexError:
        return None
    base = _token_reg(base_token)
    if base not in _FRAME_REGS:
        return None
    inner = operands.split("[", 1)[1].split("]", 1)[0]
    for token in _split_operands(inner):
        if token.startswith("#"):
            try:
                return (base, int(token[1:], 0))
            except ValueError:
                return None
        if _token_reg(token) is not None and token != base_token:
            return None  # register offset: unkeyable
    return (base, 0)


def _tracked_in(tokens: list, tracked: set) -> set:
    found = set()
    for reg in tracked:
        if any(name in tokens for name in _reg_names(reg)):
            found.add(reg)
    return found


def _reloaded_slots(func_words: dict) -> set:
    """Frame-relative stack slots read back anywhere in the function.

    A store to a slot in this set is a spill (the function reads its own
    value back); a store to any other stack slot is read by an invisible
    party (the SjLj resume path, a callee through the frame address) and
    therefore an escape. Store instructions are excluded so that a slot
    which is only ever written counts as never reloaded.
    """
    slots = set()
    for insn in func_words.values():
        if insn.id in _STORES | _MULTI_STORE:
            continue
        slot = _stack_slot(insn.op_str)
        if slot is not None:
            slots.add(slot)
    return slots


def _classify_literal_use(image, func_words: dict, load_addr: int, reg: int,
                          noreturn: frozenset | None = None,
                          reloaded: frozenset | None = None) -> tuple:
    """Bounded intra-procedural use analysis for a loaded register.

    Tracks the loaded value through register copies and address arithmetic,
    kills it on redefinition and call clobbers, and reports how the value is
    used. Returns (verdict, evidence).
    """
    if reg is None or reg < 0:
        return ("VALUE", "VFP destination holds floating-point constants only", [])
    if reg in (13, 15):
        return ("VALUE", "literal loaded into sp/pc is not a data pointer", [])
    tracked = {reg}
    seen = set()
    queue = [load_addr + 4]
    budget = 600
    exhausted = False
    saw_call_arg = 0
    saw_store = 0
    saw_return = 0
    saw_value_use = False
    saw_index = False
    store_sites: list = []
    # Stack-slot spill tracking: spills survive calls by design, so a store
    # to [sp|r7|fp, #off] remembers the slot and a later load from the same
    # slot resumes tracking; any overwrite of the slot clears it. Without
    # this, spilled integers look like escaping pointers (false weak uses
    # that spawn bogus landing pads) and spilled pointers lose their
    # dereference proof. Keyed by (base register, offset).
    spilled: set = set()
    while queue and budget > 0 and (tracked or spilled):
        budget -= 1
        if budget == 0:
            exhausted = True
        addr = queue.pop(0)
        if addr in seen or addr not in func_words:
            continue
        seen.add(addr)
        insn = func_words[addr]
        mnemonic, operands = insn.mnemonic, insn.op_str
        tokens = _split_operands(operands)
        used = _tracked_in(tokens, tracked)
        first_reg = _token_reg(tokens[0]) if tokens else None
        # A reload from a slot holding our spilled value resumes tracking.
        # (Slots are monotonic: stale entries can only over-approximate, and
        # every pointer verdict is target-audited downstream, while clearing
        # could silently lose a real pointer. See pointers.py.)
        resumed = False
        if spilled and insn.id in _WORD_LOADS | _id("ARM_INS_LDRD"):
            slot = _stack_slot(operands)
            if slot is not None and (
                slot in spilled
                or (slot[0], slot[1] - 4) in spilled  # second word of strd
            ):
                for dest in {first_reg, _token_reg(tokens[1]) if len(tokens) > 1 else None}:
                    if dest is not None and dest not in (13, 15) and len(tracked) < 8:
                        tracked.add(dest)
                        resumed = True
                        used = used | {dest}
        if spilled and insn.id not in _MULTI_LOAD | _MULTI_STORE:
            bracket_base = None
            if "[" in tokens:
                following = tokens[tokens.index("[") + 1:]
                bracket_base = _token_reg(following[0]) if following else None
            leaked = any(
                reg in _FRAME_REGS and reg != bracket_base
                for reg in (_token_reg(token) for token in tokens)
                if reg is not None
            )
            if leaked:
                # A frame register is used as something other than the base
                # of a stack access (`add r0, sp, #x`, `str sp, [...]`) while
                # our value sits in a slot: someone else may read the slot.
                # Record the escape but keep tracking reloads.
                saw_store = saw_store or addr
        if used and insn.id in _DERIVING | _id("ARM_INS_RSB") and "pc" in tokens:
            if len(tokens) > 1 and tokens[1] == "pc":
                # `add/sub dst, pc, tracked`: the loaded value is a
                # PC-relative *offset* (crt start, PIC data access, dispatch
                # add-pc-pc), not an address. Offsets are invariant under
                # image sliding.
                return ("VALUE", f"pc-relative offset combined with pc at {addr:#x} "
                                 f"({mnemonic} {operands})", store_sites)
            if first_reg == 15:
                # `add/sub pc, tracked, ...`: branch through a tracked base.
                return ("POINTER", f"value branched through at {addr:#x} "
                                   f"({mnemonic} {operands})", store_sites)

        if insn.id in _CALL:
            if tracked & {0, 1, 2, 3}:
                saw_call_arg = saw_call_arg or addr
            tracked -= _CALL_CLOBBERED
            # Spilled stack slots survive calls (callee-saved by contract);
            # only the registers die.
            if not tracked and not spilled:
                break
            if noreturn and _branch_target(insn) in noreturn:
                break  # noreturn call ends the path; bytes after are data
            queue.append(addr + 4)
            continue
        # `mov lr, pc; ldr pc, [...]` — indirect call through a vtable or a
        # computed target. Treated like a call (arguments escape, results
        # clobber) and the fallthrough stays reachable.
        if insn.id in _id("ARM_INS_LDR") and first_reg == 15 and "[sp" not in operands:
            if tracked & {0, 1, 2, 3}:
                saw_call_arg = saw_call_arg or addr
            tracked -= _CALL_CLOBBERED
            if not tracked:
                break
            queue.append(addr + 4)
            continue
        if _is_return(insn):
            if tracked & {0, 1}:
                saw_return = saw_return or addr
            continue
        if not _is_unconditional(insn) and (
            (insn.id in _id("ARM_INS_BX") and "lr" in tokens)
            or (insn.id in _id("ARM_INS_POP") and 15 in {_token_reg(t) for t in tokens})
            or (insn.id in _id("ARM_INS_MOV") and first_reg == 15)
            or (insn.id in _id("ARM_INS_LDR") and first_reg == 15)
        ):
            # Conditional return: a loaded value in r0/r1 still escapes to the
            # caller on the taken path, but the fallthrough stays reachable.
            if tracked & {0, 1}:
                saw_return = saw_return or addr
            queue.append(addr + 4)
            continue

        if used:
            if insn.id in _id("ARM_INS_BX", "ARM_INS_BLX"):
                return ("POINTER", f"value branched to at {addr:#x} ({mnemonic} {operands})",
                        store_sites)
            if insn.id in _STORES and (
                first_reg in tracked
                or (
                    insn.id in _id("ARM_INS_STRD")
                    and len(tokens) > 1
                    and _token_reg(tokens[1]) in tracked
                )
            ):
                if insn.id in _WORD_STORES | _id("ARM_INS_STRD"):
                    slot = _stack_slot(operands)
                    base = _token_reg(tokens[tokens.index("[") + 1]) \
                        if "[" in tokens and len(tokens) > tokens.index("[") + 1 \
                        else None
                    store_sites.append((addr, base))
                    if slot is not None and reloaded is not None and slot in reloaded:
                        # A spill the function reads back is contained; the
                        # reload resumes tracking.
                        spilled.add(slot)
                        if insn.id in _id("ARM_INS_STRD"):
                            spilled.add((slot[0], slot[1] + 4))
                    else:
                        # A store anywhere else -- globals, heap, objects, or
                        # a stack slot nobody reloads (an SjLj context slot,
                        # read by the invisible resume path) -- is an escape.
                        saw_store = saw_store or addr
                else:
                    # Byte/halfword stores keep only a fragment: integer use.
                    saw_value_use = True
            elif insn.id in _MULTI_STORE:
                base = _token_reg(tokens[0]) if tokens else None
                store_sites.append((addr, base))
                saw_store = saw_store or addr
            elif "[" in operands and "]" in operands:
                inner = operands.split("[", 1)[1].split("]", 1)[0]
                bracket_regs = [
                    reg for reg in
                    (_token_reg(token) for token in _split_operands(inner))
                    if reg is not None
                ]
                base = bracket_regs[0] if bracket_regs else None
                index = bracket_regs[1:]
                if base in tracked and (
                    first_reg not in tracked or first_reg in bracket_regs
                ):
                    # A tracked register used as the memory base is a genuine
                    # dereference, including `ldr r1, [r1]` (use the old
                    # value, then redefine). A tracked register in
                    # destination position only (`ldr r1, [pc]`) is a
                    # redefinition, handled by the kill rule below.
                    return ("POINTER", f"value dereferenced at {addr:#x} "
                                       f"({mnemonic} {operands})", store_sites)
                if any(reg in tracked for reg in index):
                    inner_tokens = _split_operands(inner)
                    scaled = any(
                        token.startswith("#") or token in
                        ("lsl", "lsr", "asr", "ror", "rrx")
                        for token in inner_tokens
                    )
                    if scaled:
                        # A shifted/immediate index (`ldr r3, [r5, ip,
                        # lsl #2]`) is integer offset arithmetic.
                        saw_value_use = True
                    else:
                        # A plain register offset (`ldr r5, [fp, r2]`) is
                        # commutative: the compiler may put the array base
                        # in either position. Flow cannot tell base from
                        # index, so the target audit decides.
                        saw_index = True
            elif insn.id in _COMPARES:
                saw_value_use = True
            elif insn.id in _COPIES and first_reg is not None and first_reg not in tracked:
                if len(tracked) < 8:
                    tracked.add(first_reg)
            elif insn.id in _DERIVING and first_reg is not None and first_reg not in tracked:
                if len(tracked) < 8:
                    tracked.add(first_reg)
            elif insn.id in _VALUE_ALU and first_reg not in tracked:
                saw_value_use = True

        # Redefinition kills tracking, unless the new value derives from a
        # tracked register (copy / address arithmetic, handled above).
        if (
            insn.id in _REDEFINING_FIRST
            and first_reg in tracked
            and addr != load_addr
            and not resumed
            and not (insn.id in _COPIES | _DERIVING and used - {first_reg})
        ):
            tracked.discard(first_reg)
            if not tracked:
                break
        if insn.id in _MULTI_LOAD:
            for token in tokens:
                killed = _token_reg(token)
                if killed in tracked:
                    tracked.discard(killed)
            if not tracked:
                break
        if "!" in tokens:  # writeback redefines the base register
            for token in tokens:
                killed = _token_reg(token)
                if killed in tracked and insn.id in _MULTI_LOAD | _MULTI_STORE | _STORES | _LITERAL_LOADS:
                    tracked.discard(killed)
            if not tracked:
                break

        target = _branch_target(insn)
        conditional = not _is_unconditional(insn)
        if target is not None:
            if conditional:
                queue.append(target)
                queue.append(addr + 4)
            else:
                queue.append(target)
        elif not _is_return(insn) and insn.id not in _id("ARM_INS_BX"):
            queue.append(addr + 4)
    escape = saw_store or saw_call_arg or saw_return
    if escape:
        if not saw_value_use:
            if saw_store:
                what, where = "stored to memory", saw_store
            elif saw_call_arg:
                what, where = "passed to call", saw_call_arg
            else:
                what, where = "returned to caller", saw_return
            return ("POINTER", f"in-image value {what} at {where:#x} with no value use",
                    store_sites)
        return (
            "AMBIGUOUS",
            f"value both escapes (at {escape:#x}) and is compared/computed "
            f"from {load_addr:#x}",
            store_sites,
        )
    if exhausted:
        # The walk ran out of budget with live state: a use beyond the
        # horizon may still exist, so this must not resolve to VALUE.
        return ("AMBIGUOUS", f"use analysis exhausted its budget from {load_addr:#x}",
                store_sites)
    if saw_index:
        return ("AMBIGUOUS", f"value used as a plain address index from {load_addr:#x}; "
                "base/index decided by target audit", store_sites)
    if saw_value_use:
        return ("VALUE", f"value only compared/computed, never dereferenced from {load_addr:#x}",
                store_sites)
    if not tracked:
        return ("VALUE", f"value dies (redefined/clobbered) without pointer use from {load_addr:#x}",
                store_sites)
    return ("AMBIGUOUS", f"no decisive register use found from {load_addr:#x}",
            store_sites)


# ---------------------------------------------------------------------------
# Recursive descent


_PC_TABLE_FORM = re.compile(r"\[pc, (\w+), lsl #2\]")

_COND_TABLE_EXTRA = {"ldrls": 1, "ldrlo": 0, "ldrcc": 0}


def _follow_pc_table(image, text, func, func_words, addr, insn):
    """Bound a PC-relative jump table; returns (base, count) or None.

    Follows only ``ldr{,ls,lo} pc, [pc, rX, lsl #2]`` with the bound taken
    from the nearest linear-predecessor ``cmp rX, #imm``: the table starts
    at dispatch+8 with bound+1 (``ls``/unconditional, index <= bound) or
    bound (``lo``, index < bound) entries. Masked indices, inverted
    conditions, a redefined index, or any control flow between the
    compare and the dispatch aborts by returning None.
    """
    match = _PC_TABLE_FORM.search(insn.op_str)
    if match is None:
        return None
    index = _token_reg(match.group(1))
    if index is None:
        return None
    if insn.mnemonic == "ldr":
        extra = 1
    elif insn.mnemonic in _COND_TABLE_EXTRA:
        extra = _COND_TABLE_EXTRA[insn.mnemonic]
    else:
        return None
    bound = None
    cursor = addr - 4
    for _ in range(6):
        prev = func_words.get(cursor)
        if prev is None:
            return None
        if prev.mnemonic == "cmp":
            tokens = _split_operands(prev.op_str)
            if (len(tokens) >= 2 and _token_reg(tokens[0]) == index
                    and tokens[1].startswith("#")):
                try:
                    bound = int(tokens[1][1:], 0)
                except ValueError:
                    return None
                break
            return None  # flags re-compared: the bound link is dead
        if prev.mnemonic in ("cmn", "tst", "teq"):
            return None
        if _branch_target(prev) is not None:
            return None
        if prev.id in _id("ARM_INS_LDM", "ARM_INS_POP"):
            return None
        if _dest_reg(prev) == index:
            return None
        cursor -= 4
    if bound is None or bound < 0 or bound > 1024:
        return None
    count = bound + extra
    if count <= 0 or count > 1025:
        return None
    base = addr + 8
    if base + 4 * count > text.address + text.size:
        return None
    for i in range(count):
        slot = base + 4 * i
        if slot in func.code_words:
            return None  # table overlaps decoded code: wrong bound
        try:
            entry = image.read_u32(slot)
        except macho.MachOError:
            return None
        if not text.address <= entry < text.address + text.size:
            return None
    return (base, count)


def disassemble_function(image: macho.Image, address: int, name: str,
                         text_limit: int | None = None) -> Function:
    """Recursively disassemble one function starting at `address`."""
    text = image.section_named("__TEXT", "__text")
    limit = text_limit or (text.address + text.size)
    func = Function(address=address, name=name)
    func_words: dict = {}
    queue = [address]
    seen = set()
    cs = require_capstone()
    try:
        noreturn = noreturn_stubs(image)
    except macho.MachOError:
        noreturn = set()
    stub_start = stub_end = None
    try:
        stubs = image.section_named("__TEXT", "__symbol_stub4")
        stub_start, stub_end = stubs.address, stubs.address + stubs.size
    except macho.MachOError:
        pass
    while queue:
        addr = queue.pop(0)
        while True:
            if addr in seen or addr in func.code_words:
                break
            if addr < text.address or addr >= limit or addr % 4 != 0:
                break
            if stub_start is not None and stub_start <= addr < stub_end:
                break  # never descend into the stub table
            try:
                raw = image.read(addr, 4)
            except macho.MachOError:
                break
            decoded = list(cs.disasm(raw, addr))
            if not decoded or decoded[0].size != 4:
                break  # data word ends this path
            insn = decoded[0]
            seen.add(addr)
            func.code_words.add(addr)
            func_words[addr] = insn
            func.instructions.append((addr, insn.mnemonic, insn.op_str, raw))
            # PC-relative literal load?
            if insn.id in _LITERAL_LOADS and "[pc" in insn.op_str:
                for op in insn.operands:
                    if op.type == ARM_OP_MEM:
                        if op.mem.index != 0:
                            break  # register-indexed: table dispatch, no pool
                        disp = op.mem.disp
                        literal = addr + 8 + disp
                        dest = _dest_reg(insn)
                        try:
                            value = image.read_u32(literal) if literal % 4 == 0 else 0
                        except macho.MachOError:
                            value = 0
                        func.literals.append(
                            LiteralUse(address=addr, register=dest,
                                       literal_address=literal, value=value,
                                       verdict="PENDING")
                        )
                        break
            target = _branch_target(insn)
            if target is not None:
                if insn.id in _BRANCH:
                    func.branches.add(target)
                    if text.address <= target < limit:
                        queue.append(target)
                    if not _is_unconditional(insn):
                        addr += 4
                        continue
                    func.ends_structured = True
                    break
                func.calls.add(target)
                func.call_sites.append(CallSite(address=addr, target=target))
                if target in noreturn:
                    func.ends_structured = True
                    break  # bytes after a noreturn call are data, not code
                addr += 4
                continue
            tokens = _split_operands(insn.op_str)
            first_reg = _token_reg(tokens[0]) if tokens else None
            if first_reg == 15 and insn.id in _id("ARM_INS_LDR", "ARM_INS_MOV", "ARM_INS_BX"):
                # Computed branch: a PC-relative table dispatch queues its
                # arms; conditional indirects also fall through. An
                # unconditional indirect call (`mov lr, pc` + `ldr pc` /
                # `bx reg`) returns, so decoding continues past it.
                func.indirect_branches.append((addr, insn.mnemonic, insn.op_str))
                if insn.id in _id("ARM_INS_LDR"):
                    table = _follow_pc_table(
                        image, text, func, func_words, addr, insn)
                    if table is not None:
                        base, count = table
                        func.jump_tables.append((base, count))
                        for i in range(count):
                            queue.append(image.read_u32(base + 4 * i))
                if _is_unconditional(insn):
                    prev = func_words.get(addr - 4)
                    if prev is not None and prev.mnemonic == "mov" \
                            and prev.op_str == "lr, pc":
                        addr += 4
                        continue
                    func.ends_structured = True
                    break
                addr += 4
                continue
            if _is_return(insn):
                func.ends_structured = True
                break
            if insn.id in _id("ARM_INS_BX"):
                # Conditional bx falls through; only the taken path returns.
                # `mov lr, pc; bx reg` is an indirect call: it returns.
                func.indirect_branches.append((addr, insn.mnemonic, insn.op_str))
                if _is_unconditional(insn):
                    prev = func_words.get(addr - 4)
                    if prev is not None and prev.mnemonic == "mov" \
                            and prev.op_str == "lr, pc":
                        addr += 4
                        continue
                    func.ends_structured = True
                    break
                addr += 4
                continue
            addr += 4
    if func.code_words:
        func.size = max(func.code_words) - min(func.code_words) + 4
    # Classify literal uses now that the full function body is known.
    reloaded = _reloaded_slots(func_words)
    for literal in func.literals:
        verdict, evidence, store_sites = _classify_literal_use(
            image, func_words, literal.address, literal.register, noreturn,
            frozenset(reloaded))
        literal.store_sites = store_sites
        if not _points_into_image(image, literal.value):
            literal.verdict = "OUT_OF_RANGE"
            literal.evidence = f"value {literal.value:#x} is outside the image"
        elif literal.value < text.address:
            # Below __text live only the Mach-O headers, which carry no
            # pointer targets (and `start`'s dyld words are bypassed anyway).
            literal.verdict = "VALUE"
            literal.evidence = (
                f"value {literal.value:#x} aims below __text into the Mach-O headers; "
                f"use: {evidence}"
            )
        else:
            literal.verdict = verdict
            literal.evidence = evidence
    return func


def disassemble_all(image: macho.Image, limit_functions: int | None = None) -> dict:
    """Disassemble every known function; returns address -> Function.

    Runs to a fixpoint: SjLj landing-pad dispatchers are discovered from the
    context-fill literals (stored in-image code addresses near the function)
    and disassembled as synthetic entries, because no direct branch reaches
    them. Spawned entries that run into undecodable bytes instead of ending
    in control flow are discarded as data misreads.

    Runs in two passes: first all known entries plus spawned landing pads,
    then a closure pass over direct branch/call targets that landed outside
    every decoded word (missed code). Mid-function labels are never spawned
    because their words are already decoded by pass one.
    """
    text = image.section_named("__TEXT", "__text")
    try:
        stubs = image.section_named("__TEXT", "__symbol_stub4")
        stub_start, stub_end = stubs.address, stubs.address + stubs.size
    except macho.MachOError:
        stub_start = stub_end = None
    try:
        stubs_by_name = {
            name: address for address, name in stub_targets(image).items()
        }
        register_stub = stubs_by_name.get("__Unwind_SjLj_Register")
    except macho.MachOError:
        register_stub = None
    entries = function_entries(image)
    ordered = sorted(entries.items())
    if limit_functions:
        ordered = ordered[:limit_functions]
    functions: dict = {}
    decoded: set = set()
    pending = list(ordered)
    seen = set()

    def process(address: int, name: str, is_pad: bool) -> None:
        if address in seen:
            return
        seen.add(address)
        if is_pad and address in decoded:
            # A queued pad that turned out to be a decoded code word (a
            # spilled integer colliding with real code) adds no coverage.
            return
        func = disassemble_function(image, address, name)
        if is_pad and not func.ends_structured:
            # Spawned entries that run into undecodable bytes instead of
            # ending in control flow are data misreads, not dispatchers.
            return
        functions[address] = func
        decoded.update(func.code_words)
        register_calls = sorted(
            site.address for site in func.call_sites
            if site.target == register_stub and register_stub is not None
        )
        if not register_calls:
            return
        first_register = register_calls[0]
        for literal in func.literals:
            if (
                literal.verdict == "POINTER"
                and "stored to memory" in literal.evidence
                and text.address <= literal.value < text.address + text.size
                and 0 <= literal.value - address < 32 * 1024
                and literal.value not in seen
                # A stored integer that coincides with a decoded code word
                # would otherwise spawn a bogus "pad" that always decodes
                # cleanly (it is the tail of a real function). Genuine
                # dispatchers sit past their function's epilogue return, in
                # words no function has decoded. When a dispatcher was
                # already decoded as another function's tail, spawning it
                # again adds no code, so skipping is lossless either way.
                and literal.value not in decoded
                # The dispatcher address is written to the SjLj context on
                # the stack frame before the context is registered. Stored
                # integers aimed at objects, globals, or reloaded spill
                # slots never qualify, no matter how close they land.
                and any(
                    base in _FRAME_REGS and site < first_register
                    for site, base in literal.store_sites
                )
            ):
                pending.append((literal.value, f"{name}$pad"))

    while pending:
        address, name = pending.pop(0)
        process(address, name, is_pad="$pad" in name)
    # Pass two: direct-flow closure over genuinely undecoded words only.
    while True:
        targets = set()
        for func in functions.values():
            targets |= func.calls | func.branches
        fresh = sorted(
            target
            for target in targets
            if text.address <= target < text.address + text.size
            and target not in decoded
            and target not in seen
            and not (stub_start is not None and stub_start <= target < stub_end)
        )
        if not fresh:
            break
        for target in fresh:
            process(target, f"flow_{target:x}", is_pad=False)
        while pending:
            address, name = pending.pop(0)
            process(address, name, is_pad="$pad" in name)
    return functions
