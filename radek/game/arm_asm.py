"""ARM32 (A32, ARM-state) instruction encoder for the native code generator.

Option B of the on-device conversion: translated code is emitted directly as
ARM machine code, with no C compiler in the loop. This module is the encoder
only. Every encoding is checked against capstone and unicorn in the tests, so
a wrong bit pattern cannot pass silently.

Scope of this slice: data processing (register and rotated-immediate operands),
word and byte loads/stores with immediate offsets, PUSH/POP, B/BL with labels,
BX/BLX. Thumb and VFP/NEON are not encoded here yet.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import IntEnum


class Reg(IntEnum):
    R0 = 0
    R1 = 1
    R2 = 2
    R3 = 3
    R4 = 4
    R5 = 5
    R6 = 6
    R7 = 7
    R8 = 8
    R9 = 9
    R10 = 10
    R11 = 11
    R12 = 12
    SP = 13
    LR = 14
    PC = 15


class Cond(IntEnum):
    EQ = 0x0
    NE = 0x1
    CS = 0x2
    CC = 0x3
    MI = 0x4
    PL = 0x5
    VS = 0x6
    VC = 0x7
    HI = 0x8
    LS = 0x9
    GE = 0xA
    LT = 0xB
    GT = 0xC
    LE = 0xD
    AL = 0xE


class DataOp(IntEnum):
    AND = 0x0
    EOR = 0x1
    SUB = 0x2
    RSB = 0x3
    ADD = 0x4
    ADC = 0x5
    SBC = 0x6
    RSC = 0x7
    TST = 0x8
    TEQ = 0x9
    CMP = 0xA
    CMN = 0xB
    ORR = 0xC
    MOV = 0xD
    BIC = 0xE
    MVN = 0xF


# Opcodes that write no destination register; they always set the flags.
_FLAGS_ONLY = {DataOp.TST, DataOp.TEQ, DataOp.CMP, DataOp.CMN}


class EncodingError(ValueError):
    """An operand that the A32 encoding cannot represent."""


def encode_rotated_immediate(value: int) -> int:
    """Return the 12-bit operand2 immediate field for ``value``.

    An A32 immediate is an 8-bit constant rotated right by an even amount.
    Values that need a different encoding are rejected, not approximated.
    """
    value &= 0xFFFFFFFF
    for rotation in range(16):
        shift = 2 * rotation
        candidate = ((value << shift) | (value >> (32 - shift))) & 0xFFFFFFFF if shift else value
        if candidate <= 0xFF:
            return (rotation << 8) | candidate
    raise EncodingError(f"0x{value:08x} is not an encodable ARM immediate")


@dataclass
class _Fixup:
    index: int  # word index of the branch instruction
    label: str
    cond: int
    link: bool


@dataclass
class ArmAssembler:
    """Accumulates A32 words; labels and branches are resolved in :meth:`finish`."""

    words: list[int] = field(default_factory=list)
    labels: dict[str, int] = field(default_factory=dict)  # label -> word index
    fixups: list[_Fixup] = field(default_factory=list)

    # -- bookkeeping ---------------------------------------------------------
    def _emit(self, word: int) -> None:
        self.words.append(word & 0xFFFFFFFF)

    def offset(self) -> int:
        """Byte offset of the next instruction from the start of this code."""
        return 4 * len(self.words)

    def label(self, name: str) -> None:
        if name in self.labels:
            raise EncodingError(f"duplicate label {name!r}")
        self.labels[name] = len(self.words)

    # -- data processing ------------------------------------------------------
    def _data(self, op: DataOp, rd: int, rn: int, operand2: int, set_flags: bool,
              cond: Cond, immediate: bool) -> None:
        s = 1 if (set_flags or op in _FLAGS_ONLY) else 0
        word = (
            (int(cond) << 28)
            | (1 << 25 if immediate else 0)
            | (int(op) << 21)
            | (s << 20)
            | (rn << 16)
            | (rd << 12)
            | operand2
        )
        self._emit(word)

    def data_imm(self, op: DataOp, rd: int, rn: int, value: int, *, set_flags: bool = False,
                 cond: Cond = Cond.AL) -> None:
        self._data(op, rd, rn, encode_rotated_immediate(value), set_flags, cond, True)

    def data_reg(self, op: DataOp, rd: int, rn: int, rm: int, *, set_flags: bool = False,
                 cond: Cond = Cond.AL) -> None:
        # Operand2 = Rm with no shift (LSL #0).
        self._data(op, rd, rn, rm, set_flags, cond, False)

    def mov(self, rd: int, rm: int, *, cond: Cond = Cond.AL) -> None:
        self.data_reg(DataOp.MOV, rd, 0, rm, cond=cond)

    def mov_imm(self, rd: int, value: int, *, cond: Cond = Cond.AL) -> None:
        self.data_imm(DataOp.MOV, rd, 0, value, cond=cond)

    def add(self, rd: int, rn: int, rm: int, *, set_flags: bool = False) -> None:
        self.data_reg(DataOp.ADD, rd, rn, rm, set_flags=set_flags)

    def add_imm(self, rd: int, rn: int, value: int, *, set_flags: bool = False) -> None:
        self.data_imm(DataOp.ADD, rd, rn, value, set_flags=set_flags)

    def sub(self, rd: int, rn: int, rm: int, *, set_flags: bool = False) -> None:
        self.data_reg(DataOp.SUB, rd, rn, rm, set_flags=set_flags)

    def sub_imm(self, rd: int, rn: int, value: int, *, set_flags: bool = False) -> None:
        self.data_imm(DataOp.SUB, rd, rn, value, set_flags=set_flags)

    def and_(self, rd: int, rn: int, rm: int) -> None:
        self.data_reg(DataOp.AND, rd, rn, rm)

    def orr(self, rd: int, rn: int, rm: int) -> None:
        self.data_reg(DataOp.ORR, rd, rn, rm)

    def eor(self, rd: int, rn: int, rm: int) -> None:
        self.data_reg(DataOp.EOR, rd, rn, rm)

    def cmp(self, rn: int, rm: int) -> None:
        self.data_reg(DataOp.CMP, 0, rn, rm)

    def cmp_imm(self, rn: int, value: int) -> None:
        self.data_imm(DataOp.CMP, 0, rn, value)

    def tst(self, rn: int, rm: int) -> None:
        self.data_reg(DataOp.TST, 0, rn, rm)

    # -- loads and stores (immediate offset) ----------------------------------
    def _mem(self, rt: int, rn: int, offset: int, *, load: bool, byte: bool,
             cond: Cond = Cond.AL) -> None:
        if not -4095 <= offset <= 4095:
            raise EncodingError(f"memory offset {offset} is outside +/-4095")
        up = 1 if offset >= 0 else 0
        word = (
            (int(cond) << 28)
            | (0b010 << 25)        # single data transfer, immediate offset
            | (1 << 24)            # P: pre-indexed
            | (up << 23)          # U
            | ((1 if byte else 0) << 22)  # B
            | ((1 if load else 0) << 20)  # L
            | (rn << 16)
            | (rt << 12)
            | abs(offset)
        )
        self._emit(word)

    def ldr(self, rt: int, rn: int, offset: int = 0) -> None:
        self._mem(rt, rn, offset, load=True, byte=False)

    def str_(self, rt: int, rn: int, offset: int = 0) -> None:
        self._mem(rt, rn, offset, load=False, byte=False)

    def ldrb(self, rt: int, rn: int, offset: int = 0) -> None:
        self._mem(rt, rn, offset, load=True, byte=True)

    def strb(self, rt: int, rn: int, offset: int = 0) -> None:
        self._mem(rt, rn, offset, load=False, byte=True)

    # -- stack ----------------------------------------------------------------
    def push(self, registers: list[int]) -> None:
        # STMDB SP!, {list}
        self._emit(0xE92D0000 | _reglist(registers))

    def pop(self, registers: list[int]) -> None:
        # LDMIA SP!, {list}
        self._emit(0xE8BD0000 | _reglist(registers))

    # -- control flow ---------------------------------------------------------
    def b(self, label: str, *, cond: Cond = Cond.AL) -> None:
        self._branch(label, cond, link=False)

    def bl(self, label: str, *, cond: Cond = Cond.AL) -> None:
        self._branch(label, cond, link=True)

    def _branch(self, label: str, cond: Cond, link: bool) -> None:
        self.fixups.append(_Fixup(len(self.words), label, int(cond), link))
        self._emit(0)  # patched in finish()

    def bx(self, rm: int, *, cond: Cond = Cond.AL) -> None:
        self._emit((int(cond) << 28) | 0x012FFF10 | rm)

    def blx(self, rm: int, *, cond: Cond = Cond.AL) -> None:
        self._emit((int(cond) << 28) | 0x012FFF30 | rm)

    def ret(self) -> None:
        self.bx(Reg.LR)

    # -- output ---------------------------------------------------------------
    def finish(self) -> bytes:
        """Resolve every branch and return the little-endian machine code."""
        words = list(self.words)
        for fixup in self.fixups:
            if fixup.label not in self.labels:
                raise EncodingError(f"undefined label {fixup.label!r}")
            target = self.labels[fixup.label]
            # A32 branch offsets are relative to PC, which reads as the instruction + 8.
            delta = 4 * (target - fixup.index) - 8
            if delta % 4 or not -(1 << 25) <= delta < (1 << 25):
                raise EncodingError(f"branch to {fixup.label!r} is out of range")
            imm24 = (delta >> 2) & 0xFFFFFF
            opcode = 0xB if fixup.link else 0xA
            words[fixup.index] = (fixup.cond << 28) | (opcode << 24) | imm24
        return b"".join(struct.pack("<I", word) for word in words)


def _reglist(registers: list[int]) -> int:
    if not registers:
        raise EncodingError("empty register list")
    mask = 0
    for register in registers:
        if not 0 <= register <= 15:
            raise EncodingError(f"bad register {register}")
        mask |= 1 << register
    return mask
