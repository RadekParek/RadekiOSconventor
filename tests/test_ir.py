import struct
import unittest
from radek.ir import *


class IRTests(unittest.TestCase):
    def test_arm64_preserved(self):
        code = struct.pack("<III", 0x52800500, 0x11000800, 0xD65F03C0)
        p = lift(code, "arm64")
        self.assertEqual(p.machine_code, code)
        self.assertEqual([i.op for i in p.blocks[0].instructions], [Op.CONST, Op.ADD, Op.RETURN])

    def test_arm32_offline_lowering(self):
        expected = struct.pack("<III", 0x52800500, 0x11000800, 0xD65F03C0)
        code = struct.pack("<III", 0xE3A00028, 0xE2800002, 0xE12FFF1E)
        self.assertEqual(lift(code, "armv7").machine_code, expected)

    def test_armv6_offline_lowering(self):
        code = struct.pack("<II", 0xE3A0002A, 0xE12FFF1E)
        self.assertEqual(lift(code, "armv6").machine_code, struct.pack("<II", 0x52800540, 0xD65F03C0))
        thumb = lift(struct.pack("<HH", 0x202A, 0x4770), "armv6", True)
        self.assertEqual(thumb.machine_code, struct.pack("<II", 0x52800540, 0xD65F03C0))

    def test_thumb_offline_lowering(self):
        p = lift(struct.pack("<HHHH", 0x2029, 0x3002, 0x3801, 0x4770), "armv7", True)
        self.assertEqual(len(p.machine_code), 16)
        self.assertEqual(p.blocks[0].instructions[2].op, Op.SUB)

    def test_thumb2_movw_movt(self):
        p = lift(struct.pack("<HHHHH", 0xF241, 0x2034, 0xF2C5, 0x6078, 0x4770), "armv7s", True)
        self.assertEqual(p.blocks[0].instructions[0].immediate, 0x1234)
        self.assertEqual(p.blocks[0].instructions[1].immediate, 0x5678)
        self.assertEqual(p.machine_code, struct.pack("<III", 0x52824680, 0x72AACF00, 0xD65F03C0))

    def test_arm_rotated_constant(self):
        p = lift(struct.pack("<II", 0xE3A004FF, 0xE12FFF1E), "armv7")
        self.assertEqual(p.blocks[0].instructions[0].immediate, 0xFF000000)

    def test_uninitialized_inputs(self):
        for code in (
            struct.pack("<I", 0xD65F03C0),
            struct.pack("<II", 0x11000420, 0xD65F03C0),
            struct.pack("<II", 0x72800020, 0xD65F03C0),
        ):
            with self.assertRaises(Unsupported):
                lift(code, "arm64")

    def test_memory_syscalls_pac_calls_branches_blocked(self):
        for word in (0xD4000001, 0xF9400000, 0xD503233F, 0x94000000, 0x14000000, 0x52800013, 0x5280001F):
            with self.subTest(word=word), self.assertRaises(Unsupported):
                lift(struct.pack("<II", word, 0xD65F03C0), "arm64")

    def test_arm64e_blocked(self):
        with self.assertRaises(Unsupported):
            lift(b"", "arm64e")

    def test_truncated_and_no_return(self):
        for code in (b"\x00", struct.pack("<I", 0x52800020)):
            with self.assertRaises(Unsupported):
                lift(code, "arm64")

    def test_thumb_it_and_branch_blocked(self):
        for word in (0xBF08, 0xE000, 0x4800):
            with self.assertRaises(Unsupported):
                lift(struct.pack("<HH", word, 0x4770), "armv7", True)

    def test_invalid_thumb2(self):
        with self.assertRaises(Unsupported):
            lift(struct.pack("<HH", 0xF240, 0x8000), "armv7", True)
        with self.assertRaisesRegex(Unsupported, "unavailable on ARMv6"):
            lift(struct.pack("<HHH", 0xF240, 0x002A, 0x4770), "armv6", True)

    def test_arithmetic_modulo_width(self):
        code = struct.pack("<IIII", 0x529FFFE0, 0x72BFFFE0, 0x11000400, 0xD65F03C0)
        p = lift(code, "arm64")
        self.assertEqual(p.machine_code, code)
