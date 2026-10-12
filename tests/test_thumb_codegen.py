"""Tests for the Thumb encoder (radek/game/thumb_asm.py).

Each encoding is checked with capstone (disassembly text) and unicorn (execution
in Thumb state). Tests skip when capstone or unicorn is not installed; CI installs
both.
"""

import unittest

from radek.game.arm_asm import Cond, Reg
from radek.game.thumb_asm import ThumbAssembler, ThumbEncodingError

try:
    import capstone
    from unicorn import UC_PROT_ALL, Uc, UC_ARCH_ARM, UC_MODE_THUMB
    from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP
except ImportError:  # pragma: no cover
    capstone = None
    Uc = None

CODE = 0x10000
STACK_TOP = 0x30F00
SENTINEL = 0x40000


def _disassemble(code: bytes) -> list[str]:
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
    return [f"{i.mnemonic} {i.op_str}".strip() for i in md.disasm(code, CODE)]


def _run(code: bytes, args: dict[int, int] | None = None):
    emu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    emu.mem_map(CODE, 0x10000, UC_PROT_ALL)
    emu.mem_map(0x30000, 0x10000, UC_PROT_ALL)
    emu.mem_map(SENTINEL & ~0xFFF, 0x1000, UC_PROT_ALL)
    emu.mem_write(CODE, code)
    emu.reg_write(UC_ARM_REG_SP, STACK_TOP)
    emu.reg_write(UC_ARM_REG_LR, SENTINEL | 1)
    for reg, value in (args or {}).items():
        emu.reg_write(reg, value)
    emu.emu_start(CODE | 1, SENTINEL, count=2000)
    return emu


@unittest.skipIf(capstone is None or Uc is None, "capstone and unicorn are required")
class ThumbEncodingTests(unittest.TestCase):
    def test_disassembly_matches_intent(self):
        asm = ThumbAssembler()
        asm.mov_imm(Reg.R0, 5)
        asm.add(Reg.R0, Reg.R0, Reg.R1)
        asm.sub_imm8(Reg.R2, 3)
        asm.push([Reg.R4], lr=True)
        asm.pop([Reg.R4], pc=True)
        asm.bx(Reg.LR)
        self.assertEqual(
            _disassemble(asm.finish()),
            ["movs r0, #5", "adds r0, r0, r1", "subs r2, #3", "push {r4, lr}", "pop {r4, pc}", "bx lr"],
        )

    def test_hi_register_mov_and_compare(self):
        asm = ThumbAssembler()
        asm.mov(Reg.R8, Reg.R0)
        asm.cmp_imm8(Reg.R3, 7)
        asm.cmp(Reg.R1, Reg.R2)
        self.assertEqual(_disassemble(asm.finish()), ["mov r8, r0", "cmp r3, #7", "cmp r1, r2"])

    def test_loads_and_stores(self):
        asm = ThumbAssembler()
        asm.ldr_imm(Reg.R0, Reg.R1, 8)
        asm.str_imm(Reg.R2, Reg.R3, 124)
        asm.ldrb_imm(Reg.R4, Reg.R5, 31)
        asm.strb_imm(Reg.R6, Reg.R7, 1)
        self.assertEqual(
            _disassemble(asm.finish()),
            ["ldr r0, [r1, #8]", "str r2, [r3, #0x7c]", "ldrb r4, [r5, #0x1f]", "strb r6, [r7, #1]"],
        )

    def test_bl_is_a_wide_branch_to_the_label(self):
        asm = ThumbAssembler()
        asm.bl("callee")  # at offset 0
        asm.nop()
        asm.nop()
        asm.nop()
        asm.label("callee")
        asm.bx(Reg.LR)
        code = asm.finish()
        # bl (4 bytes) + three nops (6 bytes) puts callee at offset 0xa.
        self.assertEqual(_disassemble(code)[:1], [f"bl #0x{CODE + 10:x}"])

    def test_bl_backward_branch_sign_extends(self):
        asm = ThumbAssembler()
        asm.label("top")
        asm.nop()
        asm.bl("top")  # offset from BL (pc = +4) back to 0
        code = asm.finish()
        self.assertEqual(_disassemble(code)[1], f"bl #0x{CODE:x}")

    def test_unconditional_and_conditional_branches(self):
        asm = ThumbAssembler()
        asm.b("end")
        asm.b("end", cond=Cond.EQ)
        asm.label("end")
        asm.bx(Reg.LR)
        self.assertEqual(_disassemble(asm.finish())[:2], ["b #0x10004", "beq #0x10004"])

    def test_loop_sums_its_argument(self):
        # r0 = n; returns sum 1..n in r0 (uses flags, a conditional branch, and an
        # unconditional back-edge).
        asm = ThumbAssembler()
        asm.mov_imm(Reg.R1, 0)
        asm.label("loop")
        asm.cmp_imm8(Reg.R0, 0)
        asm.b("done", cond=Cond.EQ)
        asm.add(Reg.R1, Reg.R1, Reg.R0)
        asm.sub_imm3(Reg.R0, Reg.R0, 1)
        asm.b("loop")
        asm.label("done")
        asm.mov(Reg.R0, Reg.R1)
        asm.bx(Reg.LR)
        emu = _run(asm.finish(), {UC_ARM_REG_R0: 10})
        self.assertEqual(emu.reg_read(UC_ARM_REG_R0), 55)

    def test_nested_call_through_push_and_pop(self):
        asm = ThumbAssembler()
        asm.push([Reg.R4], lr=True)
        asm.mov_imm(Reg.R4, 7)
        asm.bl("helper")
        asm.add(Reg.R0, Reg.R0, Reg.R4)
        asm.pop([Reg.R4], pc=True)
        asm.label("helper")
        asm.add_imm8(Reg.R0, 1)
        asm.bx(Reg.LR)
        emu = _run(asm.finish(), {UC_ARM_REG_R0: 100})
        self.assertEqual(emu.reg_read(UC_ARM_REG_R0), 108)

    def test_literal_pool_load(self):
        asm = ThumbAssembler()
        asm.ldr_literal(Reg.R0, "value")
        asm.bx(Reg.LR)
        asm.label("value")
        asm.word(0xDEADBEEF)
        emu = _run(asm.finish())
        self.assertEqual(emu.reg_read(UC_ARM_REG_R0), 0xDEADBEEF)

    def test_stack_frame_with_sub_sp(self):
        asm = ThumbAssembler()
        asm.sub_sp(2)
        asm.mov_imm(Reg.R0, 9)
        asm.str_sp(Reg.R0, 4)
        asm.ldr_sp(Reg.R1, 4)
        asm.add_sp(2)
        asm.mov(Reg.R0, Reg.R1)
        asm.bx(Reg.LR)
        emu = _run(asm.finish())
        self.assertEqual(emu.reg_read(UC_ARM_REG_R0), 9)


class ThumbRangeTests(unittest.TestCase):
    def test_out_of_range_operands_are_rejected(self):
        asm = ThumbAssembler()
        with self.assertRaises(ThumbEncodingError):
            asm.mov_imm(Reg.R0, 256)
        with self.assertRaises(ThumbEncodingError):
            asm.ldr_imm(Reg.R0, Reg.R1, 6)  # not a multiple of 4
        with self.assertRaises(ThumbEncodingError):
            asm.mov_imm(Reg.R8, 1)  # high register in a low-register form

    def test_undefined_label_and_far_conditional_branch_fail(self):
        asm = ThumbAssembler()
        asm.b("nowhere")
        with self.assertRaises(ThumbEncodingError):
            asm.finish()

        far = ThumbAssembler()
        far.b("end", cond=Cond.NE)
        for _ in range(200):
            far.nop()
        far.label("end")
        with self.assertRaises(ThumbEncodingError):
            far.finish()


if __name__ == "__main__":
    unittest.main()
