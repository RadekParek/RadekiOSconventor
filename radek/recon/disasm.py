"""Machine code decoding, function discovery and control-flow reconstruction.

This module performs **static decoding only**. The machine code is never
executed by this analyzer; instructions are decoded into records and
basic blocks that later stages turn into a readable listing.

Coverage is deliberately honest. ARM64 is decoded in depth; ARMv7/Thumb and
Thumb-2 are decoded for the common encodings; anything else is reported as
``unknown`` with the raw encoding word rather than being silently skipped.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from .image import MachOImage

REGISTERS = [f"x{i}" for i in range(31)] + ["sp"]
CONDITIONS = [
    "eq",
    "ne",
    "cs",
    "cc",
    "mi",
    "pl",
    "vs",
    "vc",
    "hi",
    "ls",
    "ge",
    "lt",
    "gt",
    "le",
    "al",
    "nv",
]

MAX_FUNCTIONS = 20000
MAX_INSTRUCTIONS_PER_FUNCTION = 4096
MAX_INSTRUCTIONS_PER_IMAGE = 400000


@dataclass
class Instr:
    address: int
    size: int
    mnemonic: str
    operands: str
    kind: str
    target: int | None = None
    dst: int | None = None
    sources: tuple[int, ...] = ()
    immediate: int | None = None
    condition: str | None = None
    writes_flags: bool = False
    reads_flags: bool = False
    raw: int = 0
    unknown: bool = False

    @property
    def text(self) -> str:
        return f"{self.mnemonic} {self.operands}".strip()

    @property
    def is_branch(self) -> bool:
        return self.kind == "branch"

    @property
    def is_call(self) -> bool:
        return self.kind == "call"

    @property
    def is_return(self) -> bool:
        return self.kind == "ret"

    @property
    def terminates(self) -> bool:
        return self.kind in ("ret", "trap", "unknown") or (
            self.kind == "branch" and self.condition is None
        )

    def report(self) -> dict:
        return {
            "address": f"0x{self.address:x}",
            "size": self.size,
            "text": self.text,
            "kind": self.kind,
            "target": f"0x{self.target:x}" if self.target is not None else None,
            "unknown": self.unknown,
        }


@dataclass
class Block:
    address: int
    instructions: list[Instr] = field(default_factory=list)
    successors: list[int] = field(default_factory=list)
    kind: str = "body"  # body | entry | exit

    @property
    def end(self) -> int:
        last = self.instructions[-1]
        return last.address + last.size


@dataclass
class Function:
    address: int
    name: str
    instructions: list[Instr] = field(default_factory=list)
    blocks: list[Block] = field(default_factory=list)
    calls: list[dict] = field(default_factory=list)
    message_sends: list[dict] = field(default_factory=list)
    strings: list[str] = field(default_factory=list)
    references: list[dict] = field(default_factory=list)
    unknown_instructions: int = 0
    complete: bool = True

    @property
    def size(self) -> int:
        if not self.instructions:
            return 0
        return sum(i.size for i in self.instructions)

    def report(self) -> dict:
        return {
            "name": self.name,
            "address": f"0x{self.address:x}",
            "instructionCount": len(self.instructions),
            "blockCount": len(self.blocks),
            "unknownInstructions": self.unknown_instructions,
            "calls": self.calls[:50],
            "messageSends": self.message_sends[:50],
            "strings": self.strings[:20],
            "references": self.references[:20],
            "complete": self.complete,
        }


# --- ARM64 --------------------------------------------------------------------


def _bits(word: int, hi: int, lo: int) -> int:
    return (word >> lo) & ((1 << (hi - lo + 1)) - 1)


def _sign(value: int, width: int) -> int:
    return value - (1 << width) if value & (1 << (width - 1)) else value


def _reg(index: int, sixty_four: bool = True) -> str:
    if index == 31:
        return "sp" if sixty_four else "wzr"
    return f"x{index}" if sixty_four else f"w{index}"


_EXTENDS = ("uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx")


def _extend_name(value: int) -> str:
    return _EXTENDS[value] if value < len(_EXTENDS) else f"uxt#{value}"


def decode_arm64(word: int, address: int) -> Instr:
    op = _bits(word, 28, 25)

    def instr(mnemonic, operands, kind, **extra) -> Instr:
        return Instr(address=address, size=4, mnemonic=mnemonic, operands=operands, kind=kind, raw=word, **extra)

    if word == 0:
        return instr("udf", "#0", "trap", unknown=True)
    if op in (0b1010, 0b1011):  # branches, exception and system
        if (word & 0xFC000000) == 0x14000000:
            target = address + _sign(_bits(word, 25, 0), 26) * 4
            return instr("b", f"0x{target:x}", "branch", target=target)
        if (word & 0xFC000000) == 0x94000000:
            target = address + _sign(_bits(word, 25, 0), 26) * 4
            return instr("bl", f"0x{target:x}", "call", target=target)
        if (word & 0xFF000010) == 0x54000000:
            target = address + _sign(_bits(word, 23, 5), 19) * 4
            condition = CONDITIONS[_bits(word, 3, 0)]
            return instr(
                f"b.{condition}",
                f"0x{target:x}",
                "branch",
                target=target,
                condition=condition,
                reads_flags=True,
            )
        if (word & 0x7F000000) in (0x34000000, 0x35000000):
            target = address + _sign(_bits(word, 23, 5), 19) * 4
            register = _reg(_bits(word, 4, 0))
            name = "cbz" if (word & 0x7F000000) == 0x34000000 else "cbnz"
            return instr(name, f"{register}, 0x{target:x}", "branch", target=target, sources=(_bits(word, 4, 0),))
        if (word & 0x7F000000) in (0x36000000, 0x37000000):
            target = address + _sign(_bits(word, 18, 5), 14) * 4
            bit = (_bits(word, 23, 19) << 5) | _bits(word, 31, 31) << 5 | _bits(word, 18, 5)
            index = _bits(word, 23, 19)
            name = "tbz" if (word & 0x7F000000) == 0x36000000 else "tbnz"
            return instr(name, f"{_reg(_bits(word, 4, 0))}, #{index}, 0x{target:x}", "branch", target=target)
        if (word & 0xFFFFFC1F) == 0xD63F0000:
            return instr("blr", _reg(_bits(word, 9, 5)), "call", sources=(_bits(word, 9, 5),))
        if (word & 0xFFFFFC1F) == 0xD61F0000:
            return instr("br", _reg(_bits(word, 9, 5)), "branch", sources=(_bits(word, 9, 5),))
        if (word & 0xFFFFFC1F) == 0xD65F0000:
            return instr("ret", _reg(_bits(word, 9, 5), False), "ret")
        if (word & 0xFF000000) == 0xD6000000:
            return instr("eret", "", "ret")
        if (word & 0xFFF00000) == 0xD4000000:
            kind = "trap"
            return instr("svc", f"#0x{_bits(word, 20, 5):x}", kind, unknown=False)
        if (word & 0xFFF00000) == 0xD5000000:
            return instr("msr/mrs", "", "system")
        if (word & 0xFFFFF01F) == 0xD503201F:
            return instr("hint", f"#0x{_bits(word, 11, 5):x}", "system")
        if (word & 0xFF000000) in (0xD5000000,):
            return instr("sys", "", "system")
        return instr(f".word", f"0x{word:08x}", "unknown", unknown=True)
    if op in (0b1000, 0b1001):  # data processing immediate
        if (word & 0x9F000000) == 0x90000000:
            target = (address & ~0xFFF) + _sign((_bits(word, 23, 5) << 2) | _bits(word, 30, 29), 21) * 4096
            return instr("adrp", f"{_reg(_bits(word, 4, 0))}, 0x{target:x}", "adrp", dst=_bits(word, 4, 0), immediate=target)
        if (word & 0x9F000000) == 0x10000000:
            target = address + _sign((_bits(word, 23, 5) << 2) | _bits(word, 30, 29), 21)
            return instr("adr", f"{_reg(_bits(word, 4, 0))}, 0x{target:x}", "adrp", dst=_bits(word, 4, 0), immediate=target)
        if (word & 0x1F800000) == 0x11000000:
            sf = bool(word & (1 << 31))
            name = "sub" if word & (1 << 30) else "add"
            if word & (1 << 29):
                name += "s"
            imm = _bits(word, 21, 10)
            shift = _bits(word, 23, 22)
            if shift:
                imm <<= 12
            return instr(
                name,
                f"{_reg(_bits(word, 4, 0), sf)}, {_reg(_bits(word, 9, 5), sf)}, #{imm}",
                "arith",
                dst=_bits(word, 4, 0),
                sources=(_bits(word, 9, 5),),
                immediate=imm,
                writes_flags=bool(word & (1 << 29)),
            )
        if (word & 0x1F800000) == 0x12000000:
            names = {0: "and", 1: "orr", 2: "eor", 3: "ands"}
            opc = _bits(word, 30, 29)
            return instr(
                names.get(opc, "logical"),
                f"{_reg(_bits(word, 4, 0), bool(word & (1 << 31)))}, {_reg(_bits(word, 9, 5), bool(word & (1 << 31)))}, #0x{_bits(word, 21, 10):x}",
                "arith",
                dst=_bits(word, 4, 0),
                sources=(_bits(word, 9, 5),),
                immediate=_bits(word, 21, 10),
                writes_flags=opc == 3,
            )
        if (word & 0x1F800000) == 0x12800000:
            names = {0: "movn", 2: "movz", 3: "movk"}
            opc = _bits(word, 30, 29)
            shift = _bits(word, 22, 21) * 16
            imm = _bits(word, 20, 5)
            return instr(
                names.get(opc, "mov"),
                f"{_reg(_bits(word, 4, 0), bool(word & (1 << 31)))}, #0x{imm:x}{f' lsl #{shift}' if shift else ''}",
                "move",
                dst=_bits(word, 4, 0),
                immediate=imm << shift,
            )
        if (word & 0x1F800000) == 0x13000000:
            names = {0: "sbfm", 1: "bfm", 2: "ubfm"}
            opc = _bits(word, 30, 29)
            return instr(
                names.get(opc, "bfm"),
                f"{_reg(_bits(word, 4, 0), bool(word & (1 << 31)))}, {_reg(_bits(word, 9, 5), bool(word & (1 << 31)))}, #{_bits(word, 21, 16)}, #{_bits(word, 15, 10)}",
                "arith",
                dst=_bits(word, 4, 0),
                sources=(_bits(word, 9, 5),),
            )
        if (word & 0x1F800000) == 0x13800000:
            return instr(
                "extr",
                f"{_reg(_bits(word, 4, 0), bool(word & (1 << 31)))}, {_reg(_bits(word, 9, 5))}, {_reg(_bits(word, 20, 16))}, #{_bits(word, 15, 10)}",
                "arith",
                dst=_bits(word, 4, 0),
                sources=(_bits(word, 9, 5), _bits(word, 20, 16)),
            )
        return instr(".word", f"0x{word:08x}", "unknown", unknown=True)
    if op in (0b0100, 0b0110, 0b1100, 0b1110):  # loads and stores
        if (word & 0x3B000000) == 0x18000000:
            target = address + _sign(_bits(word, 23, 5), 19) * 4
            opc = _bits(word, 31, 30)
            name = {0: "ldr", 1: "ldrsw", 2: "prfm", 3: "ldr"}.get(opc, "ldr")
            return instr(
                name,
                f"{_reg(_bits(word, 4, 0), opc != 2)}, 0x{target:x}",
                "loadlit",
                dst=_bits(word, 4, 0),
                target=target,
            )
        if (word & 0x3E000000) == 0x28000000:
            load = bool(word & (1 << 22))
            name = "ldp" if load else "stp"
            imm = _sign(_bits(word, 21, 15), 7) * (8 if _bits(word, 31, 31) else 4)
            return instr(
                name,
                f"{_reg(_bits(word, 4, 0))}, {_reg(_bits(word, 14, 10))}, [{_reg(_bits(word, 9, 5))}, #{imm}]",
                "load" if load else "store",
                dst=_bits(word, 4, 0) if load else None,
                sources=(_bits(word, 9, 5), _bits(word, 4, 0), _bits(word, 14, 10)),
                immediate=imm,
            )
        opc = _bits(word, 23, 22)
        load = opc != 0
        name = "ldr" if load else "str"
        base = _bits(word, 9, 5)
        if (word & 0x3B000000) == 0x39000000:
            imm = _bits(word, 21, 10) * (1 << _bits(word, 31, 30))
        elif (word & 0x3B200C00) == 0x38000000:
            imm = _sign(_bits(word, 20, 12), 9)
        elif (word & 0x3B200C00) in (0x38000400, 0x38000C00):
            imm = _sign(_bits(word, 20, 12), 9)
            name += " (pre)" if (word & 0x3B200C00) == 0x38000400 else " (post)"
        elif (word & 0x3B200C00) == 0x38200800:
            offset = _bits(word, 20, 16)
            return instr(
                name,
                f"{_reg(_bits(word, 4, 0))}, [{_reg(base)}, {_reg(offset)}]",
                "load" if load else "store",
                dst=_bits(word, 4, 0) if load else None,
                sources=(base, offset, _bits(word, 4, 0)),
            )
        else:
            return instr(".word", f"0x{word:08x}", "unknown", unknown=True)
        return instr(
            name,
            f"{_reg(_bits(word, 4, 0))}, [{_reg(base)}, #{imm}]",
            "load" if load else "store",
            dst=_bits(word, 4, 0) if load else None,
            sources=(base,),
            immediate=imm,
        )
    if op in (0b0101, 0b1101):  # data processing register
        if (word & 0x1F000000) == 0x0B000000:
            # add/sub (shifted register), including the cmp/cmn aliases (Rd == xzr).
            sf = bool(word & (1 << 31))
            sub = bool(word & (1 << 30))
            sets = bool(word & (1 << 29))
            rd = _bits(word, 4, 0)
            rn = _bits(word, 9, 5)
            rm = _bits(word, 20, 16)
            shift = _bits(word, 23, 22)
            amount = _bits(word, 15, 10)
            if sets and rd == 31:
                return instr(
                    "cmp" if sub else "cmn",
                    f"{_reg(rn, sf)}, {_reg(rm, sf)}",
                    "compare",
                    sources=(rn, rm),
                    writes_flags=True,
                )
            suffixes = ("lsl", "lsr", "asr", "ror")
            suffix = f", {suffixes[shift]} #{amount}" if amount else ""
            return instr(
                ("sub" if sub else "add") + ("s" if sets else ""),
                f"{_reg(rd, sf)}, {_reg(rn, sf)}, {_reg(rm, sf)}{suffix}",
                "arith",
                dst=rd,
                sources=(rn, rm),
                writes_flags=sets,
                immediate=amount if not shift else None,
                condition=None,
            )
        if (word & 0x1F000000) == 0x0A000000:
            names = {0: "and", 1: "bic", 2: "orr", 3: "orn", 4: "eor", 5: "eon", 6: "ands", 7: "bics"}
            opc = (_bits(word, 30, 29) << 1) | _bits(word, 21, 21)
            wide = bool(word & (1 << 31))
            rd, rn, rm = _bits(word, 4, 0), _bits(word, 9, 5), _bits(word, 20, 16)
            name = names.get(opc, "logical")
            operands = f"{_reg(rd, wide)}, {_reg(rn, wide)}, {_reg(rm, wide)}"
            if rn == 31 and opc == 2:                # ORR Wd, WZR, Wm  ==  MOV Wd, Wm
                name, operands = "mov", f"{_reg(rd, wide)}, {_reg(rm, wide)}"
            elif rn == 31 and opc == 3:              # ORN Wd, WZR, Wm  ==  MVN Wd, Wm
                name, operands = "mvn", f"{_reg(rd, wide)}, {_reg(rm, wide)}"
            if name == "mov":
                return instr(name, operands, "move", dst=rd, sources=(rm,))
            return instr(
                name,
                operands,
                "arith",
                dst=_bits(word, 4, 0),
                sources=(_bits(word, 9, 5), _bits(word, 20, 16)),
                writes_flags=opc in (6, 7),
            )
        if (word & 0x7F200000) == 0x2B000000:
            # add/sub (extended register), including cmp/cmn aliases.
            sf = bool(word & (1 << 31))
            sub = bool(word & (1 << 30))
            sets = bool(word & (1 << 29))
            rd = _bits(word, 4, 0)
            rn = _bits(word, 9, 5)
            rm = _bits(word, 20, 16)
            extend = _bits(word, 15, 13)
            amount = _bits(word, 12, 10)
            suffix = f", {_extend_name(extend)} #{amount}" if amount else (f", {_extend_name(extend)}" if extend else "")
            if sets and rd == 31:
                return instr(
                    "cmp" if sub else "cmn",
                    f"{_reg(rn, sf)}, {_reg(rm, sf)}{suffix}",
                    "compare",
                    sources=(rn, rm),
                    writes_flags=True,
                )
            return instr(
                ("sub" if sub else "add") + ("s" if sets else ""),
                f"{_reg(rd, sf)}, {_reg(rn, sf)}, {_reg(rm, sf)}{suffix}",
                "arith",
                dst=rd,
                sources=(rn, rm),
                writes_flags=sets,
                immediate=amount << 0 if not amount else None,
            )
        if (word & 0x7F200000) == 0x1A800000:
            names = {0: "csel", 1: "csinc", 2: "csinv", 3: "csneg"}
            opc = _bits(word, 30, 30) << 1 | _bits(word, 10, 10)
            return instr(
                names.get(opc, "csel"),
                f"{_reg(_bits(word, 4, 0), bool(word & (1 << 31)))}, {_reg(_bits(word, 9, 5))}, {_reg(_bits(word, 20, 16))}, {CONDITIONS[_bits(word, 15, 12)]}",
                "select",
                dst=_bits(word, 4, 0),
                sources=(_bits(word, 9, 5), _bits(word, 20, 16)),
                reads_flags=True,
                condition=CONDITIONS[_bits(word, 15, 12)],
            )
        if (word & 0x7FE00000) == 0x1A400000:
            name = "ccmn" if word & (1 << 30) else "ccmp"
            return instr(
                name,
                f"{_reg(_bits(word, 9, 5))}, #{_bits(word, 20, 16)}, #{_bits(word, 11, 10)}, {CONDITIONS[_bits(word, 15, 12)]}",
                "compare",
                writes_flags=True,
                reads_flags=True,
                condition=CONDITIONS[_bits(word, 15, 12)],
            )
        if (word & 0x7F000000) == 0x1B000000:
            names = {
                0: "madd",
                1: "msub",
                0x20: "smaddl",
                0x21: "smsubl",
                0x40: "umaddl",
                0x41: "umsubl",
                0x80: "smulh",
                0xC0: "umulh",
            }
            wide = bool(word & (1 << 31))
            key = (_bits(word, 30, 29) << 5) | _bits(word, 21, 21) << 4 | 0
            name = names.get(key if key in names else _bits(word, 21, 21) * 0x20 + _bits(word, 30, 29), "madd")
            if _bits(word, 14, 10) == 31 and name == "madd":
                name = "mul"          # MUL Wd, Wn, Wm == MADD Wd, Wn, Wm, WZR
            return instr(
                name,
                (
                    f"{_reg(_bits(word, 4, 0), wide)}, {_reg(_bits(word, 9, 5), wide)}, "
                    f"{_reg(_bits(word, 20, 16), wide)}"
                    if _bits(word, 14, 10) == 31 and name == "mul"
                    else f"{_reg(_bits(word, 4, 0), wide)}, {_reg(_bits(word, 9, 5), wide)}, "
                    f"{_reg(_bits(word, 20, 16), wide)}, {_reg(_bits(word, 14, 10), wide)}"
                ),
                "arith",
                dst=_bits(word, 4, 0),
                sources=(_bits(word, 9, 5), _bits(word, 20, 16)),
            )
        if (word & 0x7FE00000) == 0x1AC00000:
            names = {2: "udiv", 3: "sdiv", 8: "lslv", 9: "lsrv", 10: "asrv", 11: "rorv"}
            opc = _bits(word, 15, 10)
            return instr(
                names.get(opc, "dp2"),
                f"{_reg(_bits(word, 4, 0), bool(word & (1 << 31)))}, {_reg(_bits(word, 9, 5), bool(word & (1 << 31)))}, {_reg(_bits(word, 20, 16), bool(word & (1 << 31)))}",
                "arith",
                dst=_bits(word, 4, 0),
                sources=(_bits(word, 9, 5), _bits(word, 20, 16)),
            )
        if (word & 0x7FE00000) == 0x5AC00000:
            names = {0: "rbit", 1: "rev16", 2: "rev32", 3: "rev", 4: "clz", 5: "cls"}
            opc = _bits(word, 15, 10)
            return instr(
                names.get(opc, "dp1"),
                f"{_reg(_bits(word, 4, 0), bool(word & (1 << 31)))}, {_reg(_bits(word, 9, 5), bool(word & (1 << 31)))}",
                "arith",
                dst=_bits(word, 4, 0),
                sources=(_bits(word, 9, 5),),
            )
        return instr(".word", f"0x{word:08x}", "unknown", unknown=True)
    if op in (0b0111, 0b1111):  # SIMD and floating point
        if (word & 0xFFE0F81F) == 0x1E202000:
            return instr("fcmp", f"{_reg(_bits(word, 9, 5), False)}, {_reg(_bits(word, 20, 16), False)}", "compare", writes_flags=True)
        if (word & 0x5F207C00) == 0x1E200400:
            return instr("fmov", f"{_reg(_bits(word, 4, 0), False)}, {_reg(_bits(word, 9, 5), False)}", "move", dst=_bits(word, 4, 0))
        if (word & 0x5F200000) == 0x1E200000:
            return instr("fop2", f"{_reg(_bits(word, 4, 0), False)}, {_reg(_bits(word, 9, 5), False)}, {_reg(_bits(word, 20, 16), False)}", "float", dst=_bits(word, 4, 0), sources=(_bits(word, 9, 5), _bits(word, 20, 16)))
        if (word & 0x5F000000) == 0x0E000000:
            return instr("simd", "", "simd")
        return instr("fp", f"0x{word:08x}", "float")
    return instr(".word", f"0x{word:08x}", "unknown", unknown=True)


# --- ARM32 / Thumb-2 ----------------------------------------------------------


def decode_arm(word: int, address: int) -> Instr:
    def instr(mnemonic, operands, kind, **extra) -> Instr:
        return Instr(address=address, size=4, mnemonic=mnemonic, operands=operands, kind=kind, raw=word, **extra)

    condition = CONDITIONS[_bits(word, 31, 28)]
    if (word & 0x0FFFFFF0) == 0x012FFF10:
        return instr("bx", f"r{_bits(word, 3, 0)}", "ret" if _bits(word, 3, 0) == 14 else "branch", condition=condition)
    if (word & 0x0F000000) == 0x0A000000:
        target = address + 8 + _sign(_bits(word, 23, 0), 24) * 4
        call = bool(word & (1 << 24))
        return instr("bl" if call else "b", f"0x{target:x}", "call" if call else "branch", target=target, condition=condition)
    if (word & 0x0FFFFFD0) == 0x012FFF30:
        return instr("blx", f"r{_bits(word, 3, 0)}", "call", condition=condition)
    if (word & 0x0E000000) == 0x04000000 and not (word & (1 << 20)) or (word & 0x0C000000) == 0x04000000:
        opcode = _bits(word, 24, 21)
        if opcode in (0b0100, 0b0010):  # ADD/SUB immediate
            name = "sub" if opcode == 0b0010 else "add"
            return instr(
                name,
                f"r{_bits(word, 15, 12)}, r{_bits(word, 19, 16)}, #{_bits(word, 11, 0)}",
                "arith",
                dst=_bits(word, 15, 12),
                sources=(_bits(word, 19, 16),),
                immediate=_bits(word, 11, 0),
                condition=condition,
                writes_flags=bool(word & (1 << 20)),
            )
    if (word & 0x0DE00000) == 0x01A00000:
        return instr("mov", f"r{_bits(word, 15, 12)}, r{_bits(word, 3, 0)}", "move", dst=_bits(word, 15, 12), sources=(_bits(word, 3, 0),), condition=condition)
    if (word & 0x0F000000) == 0x05000000 or (word & 0x0E000000) == 0x04000000:
        load = bool(word & (1 << 20))
        return instr(
            "ldr" if load else "str",
            f"r{_bits(word, 15, 12)}, [r{_bits(word, 19, 16)}, #{_bits(word, 11, 0)}]",
            "load" if load else "store",
            dst=_bits(word, 15, 12) if load else None,
            sources=(_bits(word, 19, 16),),
            immediate=_bits(word, 11, 0),
            condition=condition,
        )
    if (word & 0x0E000000) == 0x08000000:
        return instr("stm/ldm", "", "store" if not word & (1 << 20) else "load", condition=condition)
    return instr(".word", f"0x{word:08x}", "unknown", unknown=True, condition=condition)


def decode_thumb(data: bytes, address: int, offset: int) -> Instr:
    if offset + 2 > len(data):
        raise ValueError("truncated Thumb instruction")
    first = struct.unpack_from("<H", data, offset)[0]
    start = offset

    def instr(mnemonic, operands, kind, size=2, **extra) -> Instr:
        return Instr(
            address=address + start, size=size, mnemonic=mnemonic, operands=operands, kind=kind, raw=first, **extra
        )

    def second() -> int:
        if start + 4 > len(data):
            raise ValueError("truncated Thumb-2 instruction")
        return struct.unpack_from("<H", data, start + 2)[0]

    if first & 0xFF87 == 0x4700:
        return instr("bx", f"r{_bits(first, 6, 3)}", "ret" if _bits(first, 6, 3) == 14 else "branch")
    if first & 0xF800 in (0x2000, 0x3000, 0x3800):
        op = {0x2000: "mov", 0x3000: "add", 0x3800: "sub"}[first & 0xF800]
        return instr(
            op,
            f"r{_bits(first, 10, 8)}, #{_bits(first, 7, 0)}",
            "move" if op == "mov" else "arith",
            dst=_bits(first, 10, 8),
            sources=() if op == "mov" else (_bits(first, 10, 8),),
            immediate=_bits(first, 7, 0),
            writes_flags=True,
        )
    if first & 0xF800 == 0x2800:
        return instr("cmp", f"r{_bits(first, 10, 8)}, #{_bits(first, 7, 0)}", "compare", sources=(_bits(first, 10, 8),), writes_flags=True)
    if first & 0xF800 == 0x4800:
        target = ((address + start) & ~3) + 4 + _bits(first, 7, 0) * 4
        return instr("ldr", f"r{_bits(first, 10, 8)}, [0x{target:x}]", "loadlit", dst=_bits(first, 10, 8), target=target)
    if first & 0xF000 in (0x6000, 0x7000, 0x8000, 0x9000):
        load = bool(first & 0x0800)
        return instr(
            "ldr" if load else "str",
            f"r{_bits(first, 2, 0)}, [r{_bits(first, 5, 3)}, #{_bits(first, 10, 6) if first & 0xF000 in (0x6000, 0x7000) else _bits(first, 10, 6) * 4}]",
            "load" if load else "store",
            dst=_bits(first, 2, 0) if load else None,
            sources=(_bits(first, 5, 3),),
        )
    if first & 0xF800 == 0xE000:
        target = (address + start) + 4 + _sign(_bits(first, 10, 0), 11) * 2
        return instr("b", f"0x{target:x}", "branch", target=target)
    if first & 0xF000 == 0xD000:
        target = (address + start) + 4 + _sign(_bits(first, 7, 0), 8) * 2
        return instr(f"b.{CONDITIONS[_bits(first, 11, 8)]}", f"0x{target:x}", "branch", target=target, condition=CONDITIONS[_bits(first, 11, 8)], reads_flags=True)
    if first & 0xFF00 == 0xB500 or first & 0xFF00 == 0xB400:
        return instr("push", "{" + ", ".join(f"r{i}" for i in range(8) if first & (1 << i)) + "}", "store")
    if first & 0xFF00 == 0xBD00 or first & 0xFF00 == 0xBC00:
        return instr("pop", "{" + ", ".join(f"r{i}" for i in range(8) if first & (1 << i)) + "}", "load")
    if first == 0xBF00:
        return instr("nop", "", "system")
    if first & 0xFF00 == 0xBF00 and first & 0x000F:
        return instr("it", CONDITIONS[_bits(first, 7, 4)], "system")
    if first & 0xF800 == 0x1C00 or first & 0xF800 == 0x1800:
        return instr("add/sub", f"r{_bits(first, 2, 0)}, r{_bits(first, 5, 3)}", "arith", dst=_bits(first, 2, 0), sources=(_bits(first, 5, 3),))
    if first & 0xF800 == 0xF000 or first & 0xF800 == 0xE800:
        nxt = second()
        if first & 0xF800 == 0xF000 and nxt & 0xD000 == 0xD000:
            hi = _bits(first, 10, 0)
            lo = _bits(nxt, 10, 0)
            target = (address + start) + 4 + _sign((hi << 12) | (lo << 1), 25)
            return instr("bl", f"0x{target:x}", "call", size=4, target=target)
        if first & 0xF800 == 0xF000 and nxt & 0x8000 == 0x9000:
            hi = _bits(first, 10, 0)
            lo = _bits(nxt, 10, 0)
            target = (address + start) + 4 + _sign((hi << 12) | (lo << 1), 25)
            return instr("b.w", f"0x{target:x}", "branch", size=4, target=target)
        if first & 0xFBF0 in (0xF240, 0xF2C0):
            imm = ((first & 0x000F) << 12) | ((first & 0x0400) >> 10 << 11) | ((nxt >> 12 & 7) << 8) | (nxt & 0xFF)
            register = _bits(nxt, 11, 8)
            top = first & 0xFBF0 == 0xF2C0
            return instr(
                "movt" if top else "movw",
                f"r{register}, #0x{imm:x}",
                "move",
                size=4,
                dst=register,
                immediate=imm << (16 if top else 0),
            )
        if first & 0xFFF0 == 0xF8D0 or first & 0xFFF0 == 0xF8C0 or first & 0xFFF0 == 0xF850 or first & 0xFFF0 == 0xF840:
            load = bool(first & 0x0010)
            return instr(
                "ldr" if load else "str",
                f"r{_bits(nxt, 15, 12)}, [r{_bits(first, 3, 0)}, #{_bits(nxt, 11, 0)}]",
                "load" if load else "store",
                size=4,
                dst=_bits(nxt, 15, 12) if load else None,
                sources=(_bits(first, 3, 0),),
                immediate=_bits(nxt, 11, 0),
            )
        if first & 0xFFF0 == 0xF100 or first & 0xFFF0 == 0xF110:
            return instr(
                "add/sub",
                f"r{_bits(nxt, 11, 8)}, r{_bits(first, 3, 0)}, #{nxt & 0xFF | (_bits(nxt, 14, 12) << 8)}",
                "arith",
                size=4,
                dst=_bits(nxt, 11, 8),
                sources=(_bits(first, 3, 0),),
            )
        return instr(".word", f"0x{first:04x} 0x{nxt:04x}", "unknown", size=4, unknown=True)
    return instr(".word", f"0x{first:04x}", "unknown", unknown=True)


# --- function discovery and CFG ----------------------------------------------


def _text_section(image: MachOImage):
    for segment in ("__TEXT", "__TEXT_EXEC"):
        section = image.section(segment, "__text")
        if section:
            return section
    candidates = image.sections_named("__text")
    return candidates[0] if candidates else None


def _thumb_starts(image: MachOImage) -> set[int]:
    return {s.value & ~1 for s in image.symbols if s.thumb}


def discover_functions(image: MachOImage) -> list[int]:
    """Collect function entry points from metadata, symbols and entry state."""
    section = _text_section(image)
    if section is None:
        return []
    starts: set[int] = set()
    for address in image.function_starts:
        if section.address <= address < section.address + section.size:
            starts.add(address)
    section_index = None
    for index, item in enumerate(image.sections, start=1):
        if item is section:
            section_index = index
            break
    for symbol in image.symbols:
        if not symbol.name or section_index is None:
            continue
        if symbol.section == section_index and section.address <= symbol.value < section.address + section.size:
            starts.add(symbol.value & ~1)
    if image.entry and section.address <= image.entry < section.address + section.size:
        starts.add(image.entry)
    for address in _thumb_starts(image):
        if section.address <= address < section.address + section.size:
            starts.add(address)
    return sorted(starts)[:MAX_FUNCTIONS]


def _decode_one(image: MachOImage, address: int, thumb: bool) -> Instr | None:
    offset = image.addr_to_offset(address)
    if offset is None:
        return None
    data = image.data
    if image.architecture == "arm64":
        if offset + 4 > len(data):
            return None
        return decode_arm64(struct.unpack_from("<I", data, offset)[0], address)
    if thumb:
        try:
            return decode_thumb(data, address, offset)
        except ValueError:
            return None
    if offset + 4 > len(data):
        return None
    return decode_arm(struct.unpack_from("<I", data, offset)[0], address)


def disassemble(image: MachOImage, budget: int = MAX_INSTRUCTIONS_PER_IMAGE) -> tuple[list[Function], dict]:
    """Recursively decode every discovered function and build its CFG."""
    section = _text_section(image)
    stats = {
        "architecture": image.architecture,
        "textBytes": section.size if section else 0,
        "decodedBytes": 0,
        "instructions": 0,
        "unknownInstructions": 0,
        "functions": 0,
        "blocks": 0,
        "truncated": False,
    }
    if section is None:
        return [], stats
    starts = discover_functions(image)
    thumb_starts = _thumb_starts(image)
    # Bound each function by the next discovered start so decoding cannot run away.
    bounds: list[tuple[int, int, bool]] = []
    for index, start in enumerate(starts):
        limit = starts[index + 1] if index + 1 < len(starts) else section.address + section.size
        bounds.append((start, limit, start in thumb_starts))
    functions: list[Function] = []
    decoded_total = 0
    for start, limit, thumb in bounds:
        if decoded_total >= budget:
            stats["truncated"] = True
            break
        name = image.name_of(start) or f"sub_{start:x}"
        function = Function(address=start, name=name)
        seen: dict[int, Instr] = {}
        work: list[int] = [start]
        visited: set[int] = set()
        while work:
            current = work.pop(0)
            if current in visited or not (start <= current < limit):
                continue
            visited.add(current)
            block = Block(address=current)
            while True:
                if len(function.instructions) >= MAX_INSTRUCTIONS_PER_FUNCTION:
                    function.complete = False
                    break
                instruction = _decode_one(image, current, thumb)
                if instruction is None:
                    break
                seen[current] = instruction
                block.instructions.append(instruction)
                function.instructions.append(instruction)
                decoded_total += 1
                stats["instructions"] += 1
                stats["decodedBytes"] += instruction.size
                if instruction.unknown:
                    function.unknown_instructions += 1
                    stats["unknownInstructions"] += 1
                current += instruction.size
                if instruction.is_call and instruction.target is not None:
                    function.calls.append(
                        {
                            "from": f"0x{instruction.address:x}",
                            "target": f"0x{instruction.target:x}",
                            "name": image.target_name(instruction.target),
                        }
                    )
                if instruction.terminates:
                    if instruction.is_branch and instruction.target is not None:
                        block.successors.append(instruction.target)
                        if start <= instruction.target < limit:
                            work.append(instruction.target)
                    if instruction.condition:
                        block.successors.append(current)
                        work.append(current)
                    break
                if instruction.is_branch and instruction.condition and instruction.target is not None:
                    block.successors.append(instruction.target)
                    work.append(instruction.target)
                    block.successors.append(current)
                    work.append(current)
                    break
                if current >= limit:
                    break
            if block.instructions:
                function.blocks.append(block)
        if not function.instructions:
            continue
        function.blocks.sort(key=lambda b: b.address)
        function.instructions.sort(key=lambda i: i.address)
        functions.append(function)
    stats["functions"] = len(functions)
    stats["blocks"] = sum(len(f.blocks) for f in functions)
    return functions, stats
