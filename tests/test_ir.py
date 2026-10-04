import struct
import unittest
from radek.ir import *
from radek.llvm_ir import emit as emit_llvm, verify as verify_llvm


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

    def test_arm32_target_abi_alias_lowers_safe_code_to_armv7(self):
        code = struct.pack("<II", 0xE3A0002A, 0xE12FFF1E)
        program = lift(code, "armv6", target_abi="armeabi-v7a")
        self.assertEqual(program.machine_code, struct.pack("<II", 0xE300002A, 0xE12FFF1E))
        self.assertEqual(program.report()["targetAbi"], "armeabi-v7a")
        self.assertEqual(program.report()["backend"], "offline-armv6-to-armv7")

        thumb_code = struct.pack("<HH", 0x202A, 0x4770)
        thumb = lift(thumb_code, "armv6", True, "armeabi-v7a")
        self.assertEqual(thumb.machine_code, struct.pack("<II", 0xE300002A, 0xE12FFF1E))
        self.assertTrue(thumb.thumb)
        self.assertTrue(thumb.report()["sourceThumb"])

    def test_arm32_callee_saved_register_writes_are_rejected(self):
        code = struct.pack("<III", 0xE3A0402A, 0xE3A0002A, 0xE12FFF1E)
        with self.assertRaisesRegex(Unsupported, "callee-saved"):
            lift(code, "armv7", target_abi="armeabi-v7a")

    def test_armv6_offline_lowering(self):
        code = struct.pack("<II", 0xE3A0002A, 0xE12FFF1E)
        self.assertEqual(lift(code, "armv6").machine_code, struct.pack("<II", 0x52800540, 0xD65F03C0))
        thumb = lift(struct.pack("<HH", 0x202A, 0x4770), "armv6", True)
        self.assertEqual(thumb.machine_code, struct.pack("<II", 0x52800540, 0xD65F03C0))

    def test_armv6_to_armv7_32bit_android_code(self):
        source = struct.pack("<II", 0xE3A0002A, 0xE12FFF1E)
        program = lift(source, "armv6", target_arch="armv7")
        expected = struct.pack("<II", 0xE300002A, 0xE12FFF1E)
        self.assertEqual(program.machine_code, expected)
        self.assertEqual(program.target_abi, "armeabi-v7a")
        self.assertEqual(program.report()["outputArchitecture"], "armv7")

    def test_armv7_output_rejects_callee_saved_and_platform_registers(self):
        for register in (4, 9, 11):
            source = struct.pack("<III", 0xE3A0002A, 0xE3A00000 | (register << 12) | 1, 0xE12FFF1E)
            with self.subTest(register=register), self.assertRaisesRegex(Unsupported, "callee-saved"):
                lift(source, "armv7", target_arch="armv7")

    def test_armv7_leaf_lowers_to_32bit_android_code(self):
        source = struct.pack("<III", 0xE3A00028, 0xE2800002, 0xE12FFF1E)
        program = lift(source, "armv7", target_arch="armv7")
        expected = struct.pack("<III", 0xE3000028, 0xE2800002, 0xE12FFF1E)
        self.assertEqual(program.machine_code, expected)
        self.assertEqual(program.target_abi, "armeabi-v7a")

    def test_thumb_armv7_leaf_lowers_to_a32_android_code(self):
        source = struct.pack("<HH", 0x202A, 0x4770)
        program = lift(source, "armv7", True, target_arch="armv7")
        self.assertEqual(program.machine_code, struct.pack("<II", 0xE300002A, 0xE12FFF1E))

    def test_thumb_offline_lowering(self):
        p = lift(struct.pack("<HHHH", 0x2029, 0x3002, 0x3801, 0x4770), "armv7", True)
        self.assertEqual(len(p.machine_code), 16)
        self.assertEqual(p.blocks[0].instructions[2].op, Op.SUB)

    def test_thumb2_movw_movt(self):
        p = lift(struct.pack("<HHHHH", 0xF241, 0x2034, 0xF2C5, 0x6078, 0x4770), "armv7s", True)
        self.assertEqual(p.blocks[0].instructions[0].immediate, 0x1234)
        self.assertEqual(p.blocks[0].instructions[1].immediate, 0x5678)
        self.assertEqual(p.machine_code, struct.pack("<III", 0x52824680, 0x72AACF00, 0xD65F03C0))

        armv7 = lift(
            struct.pack("<HHHHH", 0xF241, 0x2034, 0xF2C5, 0x6078, 0x4770),
            "armv7s",
            True,
            target_arch="armv7",
        )
        self.assertEqual(armv7.machine_code, struct.pack("<III", 0xE3010234, 0xE3450678, 0xE12FFF1E))
        self.assertEqual(armv7.target_abi, "armeabi-v7a")

    def test_arm_rotated_constant(self):
        source = struct.pack("<II", 0xE3A004FF, 0xE12FFF1E)
        p = lift(source, "armv7")
        self.assertEqual(p.blocks[0].instructions[0].immediate, 0xFF000000)
        armv7 = lift(source, "armv7", target_arch="armv7")
        self.assertEqual(
            armv7.machine_code,
            struct.pack("<III", 0xE3000000, 0xE34F0F00, 0xE12FFF1E),
        )

        add = lift(
            struct.pack("<III", 0xE3A01001, 0xE28104FF, 0xE12FFF1E),
            "armv7",
            target_arch="armv7",
        )
        self.assertEqual(add.machine_code, struct.pack("<III", 0xE3001001, 0xE28104FF, 0xE12FFF1E))

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

    def test_closed_leaf_emits_textual_llvm_ir(self):
        code = struct.pack("<III", 0x52800500, 0x11000800, 0xD65F03C0)
        text = emit_llvm(lift(code, "arm64"))
        self.assertIn('target triple = "aarch64-unknown-linux-android"', text)
        self.assertIn("define i32 @radek_lifted()", text)
        self.assertIn("%v0 = add i32 0, 40", text)
        self.assertIn("%v1 = add i32 %v0, 2", text)
        self.assertIn("ret i32 %v1", text)
        self.assertIn("not a complete Android game port", text)

    def test_movk_llvm_lift_uses_masked_halfword_replacement(self):
        code = struct.pack("<IIII", 0x529FFFE0, 0x72BFFFE0, 0x11000400, 0xD65F03C0)
        text = emit_llvm(lift(code, "arm64"))
        self.assertIn("and i32 %v0, 65535", text)
        self.assertIn("or i32 %v1, -65536", text)
        self.assertIn("add i32 %v2, 1", text)

    def test_llvm_emitter_rejects_invalid_names_and_unsupported_programs(self):
        program = lift(struct.pack("<II", 0x52800500, 0xD65F03C0), "arm64")
        with self.assertRaisesRegex(Unsupported, "function name"):
            emit_llvm(program, "bad name")
        with self.assertRaisesRegex(Unsupported, "no verified lowering"):
            unsupported = Program("arm64", [Block(0, [Instruction(Op.LOAD, 0)])], b"", 4)
            emit_llvm(unsupported)

    def test_optional_llvm_as_verifier_reports_missing_executable(self):
        text = emit_llvm(lift(struct.pack("<II", 0x52800500, 0xD65F03C0), "arm64"))
        result = verify_llvm(text, assembler="/path/that/does/not/exist/llvm-as")
        self.assertEqual(result["status"], "FAILED")
        self.assertIn("No such file", result["message"])
