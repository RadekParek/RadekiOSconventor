"""Relocate ARM (A32) machine code from one address to another.

The game's ARMv6 code is already machine code. To run it as native code on the
phone without a C compiler, a function can be copied to a new address, and only
the instructions that depend on their address are rewritten:

* B / BL / BLX (immediate) to a target inside the copied block keep working
  because the whole block moves together. Targets outside the block are
  redirected through ``external_map`` (old absolute address -> new absolute
  address, typically an import or a relocated neighbour).
* LDR/STR/LDRB/STRB with PC as the base (literal pools) keep working inside the
  block. Outside the block they are rewritten to a pool appended after the code
  that holds the same word the original load would read. This is only correct
  for read-only constants, so the caller must pass only pools that are constant.

Anything that reads PC in another way (ADR-like arithmetic, computed table
dispatch, PC as a stored value) cannot be relocated by copying, and raises
:class:`RelocationError` with the address and instruction. Indirect branches
through registers are copied as is and listed in the result, because their
targets are absolute values the caller must map.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from .disasm import require_capstone

_PC = 15


class RelocationError(Exception):
    """An instruction whose meaning depends on its address; not guessed."""


@dataclass
class Relocated:
    code: bytes  # bytes to place at new_base
    new_base: int
    pool_base: int  # address of the appended literal pool (new space)
    indirect_branches: list = field(default_factory=list)  # old addresses
    problems: list = field(default_factory=list)  # (old address, reason) when not strict


def _words(code: bytes):
    for off in range(0, len(code) - len(code) % 4, 4):
        yield off, struct.unpack_from("<I", code, off)[0]


def _literal_target(word: int, addr: int) -> int | None:
    """Target address of a PC-based load/store with an immediate offset, or None.

    Covers LDR/STR/LDRB/STRB (imm12 offset) and VLDR/VSTR (imm8 * 4 offset),
    pre-indexed without writeback, with Rn == PC. This is how the compiler encodes
    literal loads, including floating-point constants.
    """
    if (word >> 16) & 0xF != _PC or not (word >> 24) & 1 or (word >> 21) & 1:
        return None
    if (word >> 25) & 0x7 == 0b010:  # LDR/STR immediate
        imm = word & 0xFFF
    elif (word >> 24) & 0xF == 0xD and (word >> 9) & 0x7 == 0b101:  # VLDR/VSTR (single/double)
        imm = (word & 0xFF) * 4
    else:
        return None
    off = imm if (word >> 23) & 1 else -imm
    return addr + 8 + off


def _is_vfp_literal(word: int) -> bool:
    return (word >> 24) & 0xF == 0xD


def _branch_target(word: int, addr: int) -> int | None:
    if (word >> 25) & 0x7 == 0b101 and (word >> 28) != 0xF:
        imm = word & 0xFFFFFF
        if imm & 0x800000:
            imm -= 0x1000000
        return addr + 8 + imm * 4
    if (word >> 25) == 0b1111101:  # BLX (immediate), unconditional
        imm = word & 0xFFFFFF
        if imm & 0x800000:
            imm -= 0x1000000
        h = (word >> 24) & 1
        return addr + 8 + imm * 4 + (2 if h else 0)
    return None


def _reads_pc_otherwise(insn) -> bool:
    """True when the instruction reads PC as an input (not a branch or literal load).

    Writing PC as the destination of a data-processing or load instruction is
    allowed: it does not depend on where the code lives. Stores, push, and any
    other use of PC as a source or base are reads and cannot be relocated.
    """
    import capstone

    mnem = insn.mnemonic
    if mnem == "mov" and len(insn.operands) == 2 and insn.operands[0].type == capstone.arm.ARM_OP_REG \
            and insn.operands[0].reg == capstone.arm.ARM_REG_LR \
            and insn.operands[1].type == capstone.arm.ARM_OP_REG \
            and insn.operands[1].reg == capstone.arm.ARM_REG_PC:
        # "mov lr, pc" is the call idiom. It yields the return address in the
        # block's own address space, which is correct after relocation.
        return False
    loads_into_registers = mnem.startswith(("ldm", "pop"))
    writes_only_dest = not mnem.startswith(("str", "stm", "push"))
    for index, op in enumerate(insn.operands):
        if op.type == capstone.arm.ARM_OP_REG and op.reg == capstone.arm.ARM_REG_PC:
            if loads_into_registers or (index == 0 and writes_only_dest):
                continue  # PC is a destination here, not a source
            return True
        if op.type == capstone.arm.ARM_OP_MEM and op.mem.base == capstone.arm.ARM_REG_PC:
            return True
    return False


def relocate_arm_block(image: bytes, start: int, end: int, *, new_base: int,
                       external_map: dict[int, int] | None = None,
                       read_word=None, strict: bool = True) -> Relocated:
    """Relocate ``image[start:end]`` (old absolute addresses) to ``new_base``.

    ``image`` is the bytes of the original image, indexed by ``addr - base``
    through ``read_word`` when given. For simplicity the function expects the
    block bytes at ``image`` (offset 0 = ``start``).
    """
    import capstone

    require_capstone()
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    md.detail = True
    external_map = external_map or {}
    size = end - start
    if size <= 0 or size % 4:
        raise RelocationError(f"block {start:#x}..{end:#x} is not a whole number of words")
    block = bytearray(image[:size])

    # Pass 1: literal pool words are data, not code. Record them so they are not decoded.
    data_offsets: set[int] = set()
    literal_refs: list[tuple[int, int]] = []  # (code offset, referenced old address)
    for off, word in _words(bytes(block)):
        addr = start + off
        target = _literal_target(word, addr)
        if target is not None:
            literal_refs.append((off, target))
            if start <= target < end:
                data_offsets.add(target - start)

    out = bytearray(block)
    indirect: list[int] = []
    problems: list[tuple[int, str]] = []

    def problem(addr: int, reason: str) -> None:
        if strict:
            raise RelocationError(reason)
        problems.append((addr, reason))

    pool: list[int] = []  # words appended after the block (external literals)
    pool_index: dict[int, int] = {}
    pool_base = new_base + size

    def pool_slot(old_addr: int) -> int:
        """Pool word offset from the new block start, for a literal read at old_addr."""
        if old_addr not in pool_index:
            if read_word is None:
                raise RelocationError(f"external literal at {old_addr:#x} needs read_word")
            pool_index[old_addr] = len(pool)
            pool.append(read_word(old_addr))
        return size + pool_index[old_addr] * 4

    for off in range(0, size, 4):
        if off in data_offsets:
            continue
        word = struct.unpack_from("<I", block, off)[0]
        addr = start + off
        new_addr = new_base + off

        lit = _literal_target(word, addr)
        if lit is not None:
            if start <= lit < end:
                continue  # inside the block: layout preserved, nothing to change
            slot = pool_slot(lit)
            new_off = slot - (off + 8)  # pool is after the block, PC-relative from new code
            u = 1 if new_off >= 0 else 0
            imm = abs(new_off)
            if _is_vfp_literal(word):
                if imm % 4 or imm > 0x3FC:
                    raise RelocationError(f"VFP pool out of range for {addr:#x}")
                word = (word & ~((1 << 23) | 0xFF)) | (u << 23) | (imm // 4)
            else:
                if imm > 0xFFF:
                    raise RelocationError(f"pool out of range for {addr:#x}")
                word = (word & ~((1 << 23) | 0xFFF)) | (u << 23) | imm
            struct.pack_into("<I", out, off, word)
            continue

        target = _branch_target(word, addr)
        if target is not None:
            if start <= target < end:
                continue  # inside block: relative offset unchanged
            if target not in external_map:
                problem(addr, f"branch at {addr:#x} to {target:#x} has no mapping")
                continue
            new_target = external_map[target]
            cond = word & 0xF0000000
            link = (word >> 24) & 1
            if (word >> 25) == 0b1111101:
                h = (new_target & 2) >> 1
                imm = ((new_target - (new_addr + 8)) - 2 * h) // 4
                word = 0xFA000000 | (h << 24) | (imm & 0xFFFFFF)
            else:
                imm = (new_target - (new_addr + 8)) // 4
                if (new_target - (new_addr + 8)) % 4:
                    raise RelocationError(f"branch at {addr:#x} target misaligned")
                word = cond | (0xA000000 | (link << 24)) | (imm & 0xFFFFFF)
            struct.pack_into("<I", out, off, word)
            continue

        insn = next(md.disasm(block[off:off + 4], addr), None)
        if insn is None:
            problem(addr, f"undecodable word {word:#010x} at {addr:#x}")
            continue
        if _reads_pc_otherwise(insn):
            problem(addr, f"{addr:#x}: {insn.mnemonic} {insn.op_str} reads PC as data")
            continue
        if (insn.mnemonic.startswith("bx") or insn.mnemonic.startswith("blx")) and insn.operands \
                and insn.operands[0].type == capstone.arm.ARM_OP_REG:
            indirect.append(addr)
        if insn.mnemonic == "mov" and len(insn.operands) == 2 and \
                insn.operands[0].type == capstone.arm.ARM_OP_REG and \
                insn.operands[0].reg == capstone.arm.ARM_REG_PC:
            indirect.append(addr)

    result = bytes(out) + b"".join(struct.pack("<I", w) for w in pool)
    return Relocated(code=result, new_base=new_base, pool_base=pool_base,
                     indirect_branches=indirect, problems=problems)
