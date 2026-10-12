"""Thumb (T32/Thumb-2 subset) instruction encoder for the native code generator.

Companion to ``arm_asm``. IOS ARMv6/ARMv7 binaries can mix ARM and Thumb code,
so the emitter needs both. This module encodes the Thumb-1 instructions the
generator uses, plus the 32-bit BL. Each encoding is verified against capstone
and unicorn in ``tests/test_thumb_codegen.py``.

Not encoded here: IT blocks, Thumb-2 data-processing, CBZ/CBNZ, VFP/NEON.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from radek.game.arm_asm import Cond, Reg


class ThumbEncodingError(ValueError):
    """Raised when an operand cannot be encoded in the requested Thumb form."""


def _low(reg: int) -> int:
    if not 0 <= reg <= 7:
        raise ThumbEncodingError(f"register r{reg} is not a low register")
    return reg


def _check_range(value: int, lo: int, hi: int, what: str) -> int:
    if not lo <= value <= hi:
        raise ThumbEncodingError(f"{what} {value} outside [{lo}, {hi}]")
    return value


@dataclass
class _Fixup:
    position: int  # halfword index of the instruction
    label: str
    kind: str  # "b8" (conditional), "b11" (unconditional), "bl" (32-bit)
    cond: int = 0


class ThumbAssembler:
    """Emits a Thumb code blob. Positions are byte offsets from the blob start."""

    def __init__(self) -> None:
        self._halfwords: list[int] = []
        self._labels: dict[str, int] = {}
        self._fixups: list[_Fixup] = []

    # -- infrastructure -------------------------------------------------

    def offset(self) -> int:
        return len(self._halfwords) * 2

    def label(self, name: str) -> None:
        if name in self._labels:
            raise ThumbEncodingError(f"duplicate label {name!r}")
        self._labels[name] = self.offset()

    def _emit16(self, value: int) -> None:
        self._halfwords.append(value & 0xFFFF)

    # -- data movement and arithmetic -----------------------------------

    def mov_imm(self, rd: int, value: int) -> None:
        """MOVS Rd, #imm8 (flags set, as in Thumb-1)."""
        _check_range(value, 0, 255, "immediate")
        self._emit16(0x2000 | (_low(rd) << 8) | value)

    def mov(self, rd: int, rm: int) -> None:
        """MOV Rd, Rm for any registers (flags untouched)."""
        d = int(rd)
        self._emit16(0x4600 | ((d >> 3) & 1) << 7 | (int(rm) << 3) | (d & 7))

    def add_imm8(self, rdn: int, value: int) -> None:
        """ADDS Rdn, #imm8."""
        _check_range(value, 0, 255, "immediate")
        self._emit16(0x3000 | (_low(rdn) << 8) | value)

    def sub_imm8(self, rdn: int, value: int) -> None:
        """SUBS Rdn, #imm8."""
        _check_range(value, 0, 255, "immediate")
        self._emit16(0x3800 | (_low(rdn) << 8) | value)

    def add_imm3(self, rd: int, rn: int, value: int) -> None:
        """ADDS Rd, Rn, #imm3."""
        _check_range(value, 0, 7, "immediate")
        self._emit16(0x1C00 | (value << 6) | (_low(rn) << 3) | _low(rd))

    def sub_imm3(self, rd: int, rn: int, value: int) -> None:
        """SUBS Rd, Rn, #imm3."""
        _check_range(value, 0, 7, "immediate")
        self._emit16(0x1E00 | (value << 6) | (_low(rn) << 3) | _low(rd))

    def add(self, rd: int, rn: int, rm: int) -> None:
        """ADDS Rd, Rn, Rm (low registers)."""
        self._emit16(0x1800 | (_low(rm) << 6) | (_low(rn) << 3) | _low(rd))

    def sub(self, rd: int, rn: int, rm: int) -> None:
        """SUBS Rd, Rn, Rm (low registers)."""
        self._emit16(0x1A00 | (_low(rm) << 6) | (_low(rn) << 3) | _low(rd))

    def _alu(self, op: int, rdn: int, rm: int) -> None:
        self._emit16(0x4000 | (op << 6) | (_low(rm) << 3) | _low(rdn))

    def and_(self, rdn: int, rm: int) -> None:
        self._alu(0x0, rdn, rm)

    def eor(self, rdn: int, rm: int) -> None:
        self._alu(0x1, rdn, rm)

    def orr(self, rdn: int, rm: int) -> None:
        self._alu(0xC, rdn, rm)

    def tst(self, rn: int, rm: int) -> None:
        self._alu(0x8, rn, rm)

    def cmp(self, rn: int, rm: int) -> None:
        self._alu(0xA, rn, rm)

    def cmp_imm8(self, rn: int, value: int) -> None:
        """CMP Rn, #imm8."""
        _check_range(value, 0, 255, "immediate")
        self._emit16(0x2800 | (_low(rn) << 8) | value)

    def nop(self) -> None:
        self._emit16(0xBF00)

    # -- memory ---------------------------------------------------------

    def ldr_imm(self, rt: int, rn: int, offset: int) -> None:
        """LDR Rt, [Rn, #offset] (offset multiple of 4, 0..124)."""
        _check_range(offset, 0, 124, "offset")
        if offset % 4:
            raise ThumbEncodingError("LDR offset must be a multiple of 4")
        self._emit16(0x6800 | ((offset // 4) << 6) | (_low(rn) << 3) | _low(rt))

    def str_imm(self, rt: int, rn: int, offset: int) -> None:
        """STR Rt, [Rn, #offset] (offset multiple of 4, 0..124)."""
        _check_range(offset, 0, 124, "offset")
        if offset % 4:
            raise ThumbEncodingError("STR offset must be a multiple of 4")
        self._emit16(0x6000 | ((offset // 4) << 6) | (_low(rn) << 3) | _low(rt))

    def ldr_sp(self, rt: int, offset: int) -> None:
        """LDR Rt, [SP, #offset] (multiple of 4, 0..1020)."""
        _check_range(offset, 0, 1020, "offset")
        if offset % 4:
            raise ThumbEncodingError("LDR offset must be a multiple of 4")
        self._emit16(0x9800 | (_low(rt) << 8) | (offset // 4))

    def str_sp(self, rt: int, offset: int) -> None:
        """STR Rt, [SP, #offset] (multiple of 4, 0..1020)."""
        _check_range(offset, 0, 1020, "offset")
        if offset % 4:
            raise ThumbEncodingError("STR offset must be a multiple of 4")
        self._emit16(0x9000 | (_low(rt) << 8) | (offset // 4))

    def ldrb_imm(self, rt: int, rn: int, offset: int) -> None:
        _check_range(offset, 0, 31, "offset")
        self._emit16(0x7800 | (offset << 6) | (_low(rn) << 3) | _low(rt))

    def strb_imm(self, rt: int, rn: int, offset: int) -> None:
        _check_range(offset, 0, 31, "offset")
        self._emit16(0x7000 | (offset << 6) | (_low(rn) << 3) | _low(rt))

    def ldr_literal(self, rt: int, label: str) -> None:
        """LDR Rt, [PC, #...] to a word-aligned label within 1020 bytes.

        Caller must place the literal word with ``word`` after an unconditional
        branch, aligned to 4 bytes.
        """
        self._fixups.append(_Fixup(len(self._halfwords), label, "lit", _low(rt)))
        self._emit16(0x4800 | (_low(rt) << 8))

    def word(self, value: int) -> None:
        """Raw 32-bit little-endian word (used for literal pools). Must be 4-aligned."""
        if self.offset() % 4:
            raise ThumbEncodingError("word data must be 4-byte aligned")
        self._emit16(value & 0xFFFF)
        self._emit16((value >> 16) & 0xFFFF)

    def push(self, registers: list[int], *, lr: bool = False) -> None:
        mask = 0
        for r in registers:
            mask |= 1 << _low(r)
        self._emit16(0xB400 | (0x100 if lr else 0) | mask)

    def pop(self, registers: list[int], *, pc: bool = False) -> None:
        mask = 0
        for r in registers:
            mask |= 1 << _low(r)
        self._emit16(0xBC00 | (0x100 if pc else 0) | mask)

    def sub_sp(self, words: int) -> None:
        """SUB SP, SP, #words*4."""
        _check_range(words, 0, 127, "stack words")
        self._emit16(0xB080 | words)

    def add_sp(self, words: int) -> None:
        """ADD SP, SP, #words*4."""
        _check_range(words, 0, 127, "stack words")
        self._emit16(0xB000 | words)

    # -- control flow ---------------------------------------------------

    def b(self, label: str, *, cond: Cond | None = None) -> None:
        if cond is None or cond == Cond.AL:
            self._fixups.append(_Fixup(len(self._halfwords), label, "b11"))
            self._emit16(0xE000)
        else:
            self._fixups.append(_Fixup(len(self._halfwords), label, "b8", int(cond)))
            self._emit16(0xD000)

    def bl(self, label: str) -> None:
        """32-bit BL (two halfwords)."""
        self._fixups.append(_Fixup(len(self._halfwords), label, "bl"))
        self._emit16(0xF000)
        self._emit16(0xF800)

    def bx(self, rm: int) -> None:
        self._emit16(0x4700 | (int(rm) << 3))

    def blx(self, rm: int) -> None:
        self._emit16(0x4780 | (int(rm) << 3))

    def ret(self) -> None:
        self.bx(Reg.LR)

    # -- output ---------------------------------------------------------

    def finish(self) -> bytes:
        for fix in self._fixups:
            if fix.label not in self._labels:
                raise ThumbEncodingError(f"undefined label {fix.label!r}")
            target = self._labels[fix.label]
            pc = fix.position * 2 + 4  # Thumb PC reads as instruction address + 4
            if fix.kind == "lit":
                literal = (target - ((fix.position * 2 + 4) & ~3))
                if literal < 0 or literal > 1020 or literal % 4:
                    raise ThumbEncodingError(f"literal {fix.label!r} out of range")
                self._halfwords[fix.position] |= literal // 4
                continue
            delta = target - pc
            if delta % 2:
                raise ThumbEncodingError("branch target is not halfword aligned")
            if fix.kind == "b8":
                imm = delta // 2
                _check_range(imm, -128, 127, f"conditional branch to {fix.label!r}")
                self._halfwords[fix.position] = 0xD000 | (fix.cond << 8) | (imm & 0xFF)
            elif fix.kind == "b11":
                imm = delta // 2
                _check_range(imm, -1024, 1023, f"branch to {fix.label!r}")
                self._halfwords[fix.position] = 0xE000 | (imm & 0x7FF)
            elif fix.kind == "bl":
                _check_range(delta, -(1 << 24), (1 << 24) - 2, f"BL to {fix.label!r}")
                o = delta & 0x1FFFFFF
                s = (o >> 24) & 1
                i1 = (o >> 23) & 1
                i2 = (o >> 22) & 1
                imm10 = (o >> 12) & 0x3FF
                imm11 = (o >> 1) & 0x7FF
                j1 = (~(i1 ^ s)) & 1
                j2 = (~(i2 ^ s)) & 1
                self._halfwords[fix.position] = 0xF000 | (s << 10) | imm10
                self._halfwords[fix.position + 1] = 0xD000 | (j1 << 13) | (1 << 12) | (j2 << 11) | imm11
            else:  # pragma: no cover - defensive
                raise ThumbEncodingError(f"unknown fixup kind {fix.kind}")
        return b"".join(struct.pack("<H", h) for h in self._halfwords)
