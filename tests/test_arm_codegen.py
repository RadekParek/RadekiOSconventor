"""Tests for the ARM32 encoder (radek/game/arm_asm.py) and ELF writer (arm_elf.py).

Each encoding is checked three ways: capstone must disassemble it to the expected
text, unicorn must execute it to the expected result, and the written shared
object must pass readelf and run from its exported symbol.
"""

import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from radek.game.arm_asm import ArmAssembler, Cond, EncodingError, Reg, encode_rotated_immediate
from radek.game.arm_elf import elf_hash, write_shared_object

try:
    import capstone
    from unicorn import UC_PROT_ALL, Uc, UC_ARCH_ARM, UC_MODE_ARM
    from unicorn.arm_const import (
        UC_ARM_REG_LR,
        UC_ARM_REG_R0,
        UC_ARM_REG_R1,
        UC_ARM_REG_R2,
        UC_ARM_REG_R3,
        UC_ARM_REG_SP,
    )
except ImportError:  # pragma: no cover - exercised only where the emulator is missing
    capstone = None
    Uc = None

CODE = 0x10000
DATA = 0x20000
STACK_TOP = 0x30F00
SENTINEL = 0x40000  # returning here stops emulation


def _run(code: bytes, args: dict[int, int] | None = None, data: bytes = b""):
    """Execute ``code`` from its entry point and return (registers, data memory)."""
    emu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    emu.mem_map(CODE, 0x1000, UC_PROT_ALL)
    emu.mem_map(DATA, 0x1000, UC_PROT_ALL)
    emu.mem_map(0x30000, 0x1000, UC_PROT_ALL)
    emu.mem_map(SENTINEL, 0x1000, UC_PROT_ALL)
    emu.mem_write(CODE, code)
    if data:
        emu.mem_write(DATA, data)
    registers = {UC_ARM_REG_R0: 0, UC_ARM_REG_R1: 0, UC_ARM_REG_R2: 0, UC_ARM_REG_R3: 0}
    for register, value in (args or {}).items():
        registers[register] = value
    for register, value in registers.items():
        emu.reg_write(register, value)
    emu.reg_write(UC_ARM_REG_SP, STACK_TOP)
    emu.reg_write(UC_ARM_REG_LR, SENTINEL)
    emu.emu_start(CODE, SENTINEL, count=100000)
    out = {name: emu.reg_read(reg) for name, reg in (("r0", UC_ARM_REG_R0), ("r1", UC_ARM_REG_R1),
                                                     ("r2", UC_ARM_REG_R2), ("r3", UC_ARM_REG_R3))}
    return out, bytes(emu.mem_read(DATA, 16))


def _disassemble(code: bytes) -> list[str]:
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    return [f"{insn.mnemonic} {insn.op_str}".strip() for insn in md.disasm(code, CODE)]


@unittest.skipIf(Uc is None or capstone is None, "unicorn and capstone are required")
class EncodingTests(unittest.TestCase):
    def test_rotated_immediates(self):
        self.assertEqual(encode_rotated_immediate(0xFF), 0xFF)
        self.assertEqual(encode_rotated_immediate(0xFF000000), (4 << 8) | 0xFF)
        with self.assertRaises(EncodingError):
            encode_rotated_immediate(0x101)  # needs more than 8 significant bits

    def test_add_returns_sum(self):
        asm = ArmAssembler()
        asm.add(Reg.R0, Reg.R0, Reg.R1)
        asm.ret()
        code = asm.finish()
        self.assertEqual(_disassemble(code), ["add r0, r0, r1", "bx lr"])
        result, _ = _run(code, {UC_ARM_REG_R0: 7, UC_ARM_REG_R1: 35})
        self.assertEqual(result["r0"], 42)

    def test_counted_loop_with_conditional_branch(self):
        asm = ArmAssembler()
        asm.mov_imm(Reg.R1, 0)
        asm.label("loop")
        asm.cmp_imm(Reg.R0, 0)
        asm.b("done", cond=Cond.EQ)
        asm.add(Reg.R1, Reg.R1, Reg.R0)
        asm.sub_imm(Reg.R0, Reg.R0, 1)
        asm.b("loop")
        asm.label("done")
        asm.mov(Reg.R0, Reg.R1)
        asm.ret()
        code = asm.finish()
        self.assertEqual(_run(code, {UC_ARM_REG_R0: 10})[0]["r0"], 55)
        self.assertEqual(_run(code, {UC_ARM_REG_R0: 0})[0]["r0"], 0)

    def test_word_and_byte_memory_access(self):
        asm = ArmAssembler()
        asm.str_(Reg.R0, Reg.R1, 0)       # store the word at DATA
        asm.ldrb(Reg.R2, Reg.R1, 1)       # byte 1 of that word
        asm.ldr(Reg.R3, Reg.R1, 0)        # read the word back
        asm.mov(Reg.R0, Reg.R2)
        asm.ret()
        code = asm.finish()
        result, memory = _run(code, {UC_ARM_REG_R0: 0x11223344, UC_ARM_REG_R1: DATA})
        self.assertEqual(result["r0"], 0x33)
        self.assertEqual(result["r3"], 0x11223344)
        self.assertEqual(memory[:4], struct.pack("<I", 0x11223344))

    def test_nested_call_and_return_through_stack(self):
        asm = ArmAssembler()
        asm.push([Reg.LR])
        asm.bl("inner")
        asm.add_imm(Reg.R0, Reg.R0, 1)
        asm.pop([Reg.PC])
        asm.label("inner")
        asm.add(Reg.R0, Reg.R0, Reg.R0)
        asm.ret()
        code = asm.finish()
        # capstone spells the PUSH encoding (STMDB SP!) as stmdb.
        self.assertEqual(_disassemble(code)[:2], ["stmdb sp!, {lr}", "bl #0x10010"])
        self.assertEqual(_run(code, {UC_ARM_REG_R0: 5})[0]["r0"], 11)

    def test_immediate_outside_encoding_is_rejected(self):
        asm = ArmAssembler()
        with self.assertRaises(EncodingError):
            asm.mov_imm(Reg.R0, 0x12345)

    def test_undefined_label_is_rejected(self):
        asm = ArmAssembler()
        asm.b("nowhere")
        with self.assertRaises(EncodingError):
            asm.finish()


@unittest.skipIf(Uc is None or capstone is None, "unicorn and capstone are required")
class SharedObjectTests(unittest.TestCase):
    def setUp(self):
        add = ArmAssembler()
        add.add(Reg.R0, Reg.R0, Reg.R1)
        add.ret()
        self.add_code = add.finish()

        sum_asm = ArmAssembler()
        sum_asm.mov_imm(Reg.R1, 0)
        sum_asm.label("loop")
        sum_asm.cmp_imm(Reg.R0, 0)
        sum_asm.b("done", cond=Cond.EQ)
        sum_asm.add(Reg.R1, Reg.R1, Reg.R0)
        sum_asm.sub_imm(Reg.R0, Reg.R0, 1)
        sum_asm.b("loop")
        sum_asm.label("done")
        sum_asm.mov(Reg.R0, Reg.R1)
        sum_asm.ret()
        self.sum_code = sum_asm.finish()

    @unittest.skipIf(shutil.which("readelf") is None, "readelf is required")
    def test_readelf_accepts_the_object(self):
        image = write_shared_object([("radek_add", self.add_code), ("radek_sum", self.sum_code)],
                                    soname="libradek_test.so")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "libradek_test.so"
            path.write_bytes(image)
            header = subprocess.run(["readelf", "-h", str(path)], capture_output=True, text=True,
                                    check=True).stdout
            dynamic = subprocess.run(["readelf", "-d", str(path)], capture_output=True, text=True,
                                     check=True).stdout
            symbols = subprocess.run(["readelf", "--dyn-syms", "-W", str(path)], capture_output=True,
                                     text=True, check=True).stdout
        self.assertIn("Type:                              DYN", header)
        self.assertIn("Machine:                           ARM", header)
        self.assertIn("libradek_test.so", dynamic)
        self.assertIn("radek_add", symbols)
        self.assertIn("radek_sum", symbols)

    def test_exported_functions_execute_from_the_object_bytes(self):
        image = write_shared_object([("radek_add", self.add_code), ("radek_sum", self.sum_code)])
        emu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        size = (len(image) + 0xFFF) & ~0xFFF
        emu.mem_map(CODE, size, UC_PROT_ALL)
        emu.mem_map(0x30000, 0x1000, UC_PROT_ALL)
        emu.mem_map(SENTINEL, 0x1000, UC_PROT_ALL)
        emu.mem_write(CODE, image)
        emu.reg_write(UC_ARM_REG_SP, STACK_TOP)
        emu.reg_write(UC_ARM_REG_LR, SENTINEL)
        emu.reg_write(UC_ARM_REG_R0, 7)
        emu.reg_write(UC_ARM_REG_R1, 35)
        emu.emu_start(CODE + self._symbol_value(image, "radek_add"), SENTINEL, count=1000)
        self.assertEqual(emu.reg_read(UC_ARM_REG_R0), 42)
        emu.reg_write(UC_ARM_REG_LR, SENTINEL)
        emu.reg_write(UC_ARM_REG_R0, 10)
        emu.emu_start(CODE + self._symbol_value(image, "radek_sum"), SENTINEL, count=10000)
        self.assertEqual(emu.reg_read(UC_ARM_REG_R0), 55)

    @staticmethod
    def _symbol_value(image: bytes, name: str) -> int:
        """Find an exported function's address by reading the dynamic symbol table."""
        ehdr = struct.unpack_from("<16sHHIIIIIHHHHHH", image, 0)
        phoff, phnum = ehdr[5], ehdr[10]
        dynamic = None
        for index in range(phnum):
            ptype, offset, *_ = struct.unpack_from("<IIIIIIII", image, phoff + index * 32)
            if ptype == 2:  # PT_DYNAMIC
                dynamic = offset
        entries = {}
        pos = dynamic
        while True:
            tag, value = struct.unpack_from("<II", image, pos)
            pos += 8
            if tag == 0:
                break
            entries[tag] = value
        symtab, strtab = entries[6], entries[5]
        hash_off = entries[4]
        nchain = struct.unpack_from("<I", image, hash_off + 4)[0]
        for index in range(1, nchain):
            name_off, value, _, _, _, _ = struct.unpack_from("<IIIBBH", image, symtab + index * 16)
            end = image.index(b"\0", strtab + name_off)
            if image[strtab + name_off:end] == name.encode("ascii"):
                return value
        raise AssertionError(f"symbol {name} not found")

    def test_hash_table_matches_symbol_hash(self):
        self.assertEqual(elf_hash(b""), 0)
        self.assertEqual(elf_hash(b"printf"), 0x077905A6)

    def test_duplicate_and_empty_inputs_are_rejected(self):
        with self.assertRaises(ValueError):
            write_shared_object([])
        with self.assertRaises(ValueError):
            write_shared_object([("a", self.add_code), ("a", self.sum_code)])


if __name__ == "__main__":
    unittest.main()
