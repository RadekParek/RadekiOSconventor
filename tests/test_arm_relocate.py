"""Tests for ARM block relocation (radek/game/arm_relocate.py).

Each test runs the original code at its old address and the relocated copy at a
new address under unicorn, and checks that both produce the same result.
"""

import struct
import unittest

from radek.game.arm_asm import ArmAssembler, Reg
from radek.game.arm_relocate import RelocationError, relocate_arm_block

try:
    from unicorn import UC_ARCH_ARM, UC_MODE_ARM, UC_PROT_ALL, Uc
    from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_SP
except ImportError:  # pragma: no cover
    Uc = None

OLD_BLOCK = 0x10000
OLD_EXTERNAL = 0x20000
NEW_BLOCK = 0x50000
NEW_EXTERNAL = 0x70000
SENTINEL = 0x90000
STACK_PAGE = 0x31000
CONSTANT_ADDR = OLD_BLOCK + 0x800  # a constant in the same page, outside the block


def _bl_word(site: int, target: int) -> int:
    return 0xEB000000 | (((target - (site + 8)) // 4) & 0xFFFFFF)


def _ldr_literal(rt: int, site: int, target: int) -> int:
    """LDR Rt, [PC, #(target - (site + 8))] with the U bit set for forward offsets."""
    off = target - (site + 8)
    u = 1 if off >= 0 else 0
    return 0xE51F0000 | (u << 23) | (rt << 12) | abs(off)


def _build_block():
    """push {r4, lr}; mov r4, r0; bl helper; ldr r1, =0x1000 (pool);
    add r0, r0, r1; bl external; ldr r2, =external constant; add r0, r0, r2;
    pop {r4, pc}; helper: add r0, r0, #1; bx lr; pool word 0x1000.

    Returns (block bytes at OLD_BLOCK, offset of the external bl,
             offset of the external literal load).
    """
    asm = ArmAssembler()
    asm.push([Reg.R4, Reg.LR])
    asm.mov(Reg.R4, Reg.R0)
    asm.bl("helper")
    ldr_pool = asm.offset()
    asm._emit(0xE51F1000)  # LDR r1, [pc, #?] - patched after finish
    asm.add(Reg.R0, Reg.R0, Reg.R1)
    bl_external = asm.offset()
    asm._emit(0xEB000000)  # BL external - patched after finish
    ldr_external = asm.offset()
    asm._emit(0xE51F2000)  # LDR r2, [pc, #?] - patched after finish
    asm.add(Reg.R0, Reg.R0, Reg.R2)
    asm.pop([Reg.R4, Reg.PC])
    asm.label("helper")
    asm.add_imm(Reg.R0, Reg.R0, 1)
    asm.ret()
    pool_in_block = asm.offset()
    asm._emit(0x1000)
    code = bytearray(asm.finish())

    struct.pack_into("<I", code, ldr_pool,
                     _ldr_literal(Reg.R1, OLD_BLOCK + ldr_pool, OLD_BLOCK + pool_in_block))
    struct.pack_into("<I", code, bl_external, _bl_word(OLD_BLOCK + bl_external, OLD_EXTERNAL))
    struct.pack_into("<I", code, ldr_external,
                     _ldr_literal(Reg.R2, OLD_BLOCK + ldr_external, CONSTANT_ADDR))
    return bytes(code), bl_external, ldr_external


def _external_bytes() -> bytes:
    """add r0, r0, #100; bx lr."""
    asm = ArmAssembler()
    asm.add_imm(Reg.R0, Reg.R0, 100)
    asm.ret()
    return asm.finish()


def _old_page(block: bytes) -> bytes:
    """One page at OLD_BLOCK: the block, and the constant at CONSTANT_ADDR."""
    page = bytearray(0x1000)
    page[:len(block)] = block
    struct.pack_into("<I", page, CONSTANT_ADDR - OLD_BLOCK, 0x33)
    return bytes(page)


def _run(memory: dict[int, bytes], start: int, args: int = 5) -> int:
    emu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    for addr, data in memory.items():
        size = (len(data) + 0xFFF) & ~0xFFF
        emu.mem_map(addr, size, UC_PROT_ALL)
        emu.mem_write(addr, data)
    emu.mem_map(STACK_PAGE, 0x1000, UC_PROT_ALL)
    emu.mem_map(SENTINEL, 0x1000, UC_PROT_ALL)
    emu.reg_write(UC_ARM_REG_SP, STACK_PAGE + 0x800)
    emu.reg_write(UC_ARM_REG_LR, SENTINEL)
    emu.reg_write(UC_ARM_REG_R0, args)
    emu.emu_start(start, SENTINEL, count=2000)
    return emu.reg_read(UC_ARM_REG_R0)


@unittest.skipIf(Uc is None, "unicorn is required")
class RelocationTests(unittest.TestCase):
    def test_relocated_block_matches_original_result(self):
        block, _, _ = _build_block()
        page = _old_page(block)
        external = _external_bytes()

        original = _run({OLD_BLOCK: page, OLD_EXTERNAL: external}, OLD_BLOCK)

        relocated = relocate_arm_block(
            page, OLD_BLOCK, OLD_BLOCK + len(block), new_base=NEW_BLOCK,
            external_map={OLD_EXTERNAL: NEW_EXTERNAL},
            read_word=lambda addr: struct.unpack_from("<I", page, addr - OLD_BLOCK)[0],
        )
        moved = _run({NEW_BLOCK: relocated.code, NEW_EXTERNAL: external}, NEW_BLOCK)

        # 5 -> helper 6 -> +0x1000 = 4102 -> external +100 = 4202 -> +0x33 = 4253
        self.assertEqual(original, 4253)
        self.assertEqual(moved, original)

    def test_unmapped_external_branch_is_refused(self):
        block, _, _ = _build_block()
        with self.assertRaises(RelocationError):
            relocate_arm_block(block, OLD_BLOCK, OLD_BLOCK + len(block),
                               new_base=NEW_BLOCK, external_map={},
                               read_word=lambda a: 0)

    def test_pc_as_data_is_refused(self):
        asm = ArmAssembler()
        asm.mov(Reg.R0, Reg.PC)  # reads the address of the code
        asm.ret()
        code = asm.finish()
        with self.assertRaises(RelocationError):
            relocate_arm_block(code, OLD_BLOCK, OLD_BLOCK + len(code), new_base=NEW_BLOCK)

    def test_store_of_pc_is_refused(self):
        asm = ArmAssembler()
        asm.push([Reg.PC])  # stores the PC value
        asm.ret()
        code = asm.finish()
        with self.assertRaises(RelocationError):
            relocate_arm_block(code, OLD_BLOCK, OLD_BLOCK + len(code), new_base=NEW_BLOCK)

    def test_writing_pc_from_lr_is_allowed(self):
        asm = ArmAssembler()
        asm.mov(Reg.PC, Reg.LR)
        code = asm.finish()
        relocated = relocate_arm_block(code, OLD_BLOCK, OLD_BLOCK + len(code), new_base=NEW_BLOCK)
        self.assertEqual(relocated.code, code)

    def test_indirect_branch_is_reported_with_its_old_address(self):
        asm = ArmAssembler()
        asm.add_imm(Reg.R0, Reg.R0, 1)
        asm.bx(Reg.R3)
        code = asm.finish()
        relocated = relocate_arm_block(code, OLD_BLOCK, OLD_BLOCK + len(code), new_base=NEW_BLOCK)
        self.assertEqual(relocated.indirect_branches, [OLD_BLOCK + 4])

    def test_non_strict_mode_collects_every_problem(self):
        asm = ArmAssembler()
        asm.mov(Reg.R0, Reg.PC)  # problem 1: reads PC as data
        asm._emit(0xEB000000)  # problem 2: bl to an unmapped target, patched below
        asm.push([Reg.PC])  # problem 3: stores PC
        asm.ret()
        code = bytearray(asm.finish())
        struct.pack_into("<I", code, 4, 0xEB000000 | 0x100000)
        relocated = relocate_arm_block(bytes(code), OLD_BLOCK, OLD_BLOCK + len(code),
                                       new_base=NEW_BLOCK, external_map={}, strict=False)
        self.assertEqual(len(relocated.problems), 3)


if __name__ == "__main__":
    unittest.main()
