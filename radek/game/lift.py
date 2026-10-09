"""Static ARMv6 -> portable C translator for the game binary.

Every decoded function becomes one C function over an explicit CPU state
(:file:`rt/cpu.h`). Translation is fail-closed: anything the emitter does
not understand raises :class:`LiftError` with the address and the decoded
instruction, so coverage gaps can never hide as miscompiles.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

from . import macho
from .disasm import require_capstone


class LiftError(Exception):
    """One untranslatable instruction; collected, never guessed."""


@dataclass
class LiftContext:
    image: macho.Image
    functions: dict  # addr -> Function
    cname: dict = field(default_factory=dict)  # func addr -> C identifier
    func_id: dict = field(default_factory=dict)  # func addr -> small nonzero id
    import_of_stub: dict = field(default_factory=dict)  # stub addr -> symbol
    import_id: dict = field(default_factory=dict)  # symbol -> id
    table_arms: dict = field(default_factory=dict)  # table base -> [arm addrs]


def sanitize(name: str) -> str:
    out = re.sub(r"[^0-9A-Za-z_]", "_", name)
    if out and out[0].isdigit():
        out = "f_" + out
    return out[:120] or "anon"


def build_context(image, functions) -> LiftContext:
    ctx = LiftContext(image=image, functions=functions)
    taken = set()
    for index, addr in enumerate(sorted(functions), start=1):
        ctx.func_id[addr] = index
        base = f"t_{sanitize(functions[addr].name)}_{addr:x}"
        name, suffix = base, 0
        while name in taken:
            suffix += 1
            name = f"{base}_{suffix}"
        taken.add(name)
        ctx.cname[addr] = name
    for stub_addr, _slot, symbol in macho.stub_map(image):
        ctx.import_of_stub[stub_addr] = symbol
    for index, symbol in enumerate(sorted(set(ctx.import_of_stub.values())), start=1):
        ctx.import_id[symbol] = len(functions) + index
    for func in functions.values():
        for base, count in func.jump_tables:
            arms = []
            for i in range(count):
                arms.append(image.read_u32(base + 4 * i))
            ctx.table_arms.setdefault(base, arms)
    return ctx


# ---------------------------------------------------------------------------
# capstone detail helpers

_CS = None


def _cs():
    global _CS
    if _CS is None:
        _CS = require_capstone()
    return _CS


def _reg_no(cs, reg_id: int) -> int:
    name = cs.reg_name(reg_id)
    if name == "sb":
        return 9
    if name == "sl":
        return 10
    if name == "fp":
        return 11
    if name == "ip":
        return 12
    if name == "sp":
        return 13
    if name == "lr":
        return 14
    if name == "pc":
        return 15
    if name.startswith("r") and name[1:].isdigit():
        return int(name[1:])
    raise LiftError(f"unexpected integer register {name}")


def _vfp_no(cs, reg_id: int) -> tuple[str, int]:
    name = cs.reg_name(reg_id)
    if name.startswith("s") and name[1:].isdigit():
        return ("s", int(name[1:]))
    if name.startswith("d") and name[1:].isdigit():
        return ("d", int(name[1:]))
    raise LiftError(f"unexpected VFP register {name}")


_CC_NAMES = {
    0: "eq", 1: "ne", 2: "hs", 3: "lo", 4: "mi", 5: "pl",
    6: "vs", 7: "vc", 8: "hi", 9: "ls", 10: "ge", 11: "lt",
    12: "gt", 13: "le", 14: "al", 15: "nv",
}


_CC_BIAS: int | None = None


def _cc(insn) -> int:
    """Architectural condition number (EQ=0 .. AL=14, NV=15).

    Capstone 5 reports the 1-based ARM_CC enum (INVALID=0, EQ=1 .. AL=15,
    NV=16) while capstone 4 reported 0-based numbers; emitting the raw
    value would guard every unconditional instruction with ``cc_pass(15)``
    (never) and shift every predicated one. Probe once and normalize.
    """
    global _CC_BIAS
    if _CC_BIAS is None:
        probe = list(_cs().disasm(bytes((0, 0, 0xA0, 0xE1)), 0))[0]
        _CC_BIAS = 1 if int(probe.cc) == 15 else 0
    try:
        raw = int(insn.cc)
    except (AttributeError, TypeError, ValueError):
        return 14
    if _CC_BIAS and raw == 0:
        return 14  # INVALID: treat as unconditional (matches disasm.py)
    cc = raw - _CC_BIAS
    if not 0 <= cc <= 15:
        raise LiftError(f"bad condition code {raw}")
    return cc


def _writes_flags(insn) -> bool:
    try:
        return bool(insn.update_flags)
    except AttributeError:
        return False


def _is_writeback(insn) -> bool:
    try:
        return bool(insn.writeback)
    except AttributeError:
        return False


# ---------------------------------------------------------------------------
# emitter

_TEMPS = "    uint32_t _t0, _t1, _t2, _t3;\n    uint64_t _t64;\n    float _f0, _f1;\n    double _d0, _d1;\n    (void)_t0; (void)_t1; (void)_t2; (void)_t3;\n    (void)_t64; (void)_f0; (void)_f1; (void)_d0; (void)_d1;\n"


class _Emitter:
    def __init__(self, ctx: LiftContext, func):
        self.ctx = ctx
        self.func = func
        self.lines: list[str] = []
        self.by_addr: dict[int, tuple] = {}
        self.raw_by_addr: dict[int, bytes] = {}
        for addr, mnemonic, op_str, raw in func.instructions:
            decoded = list(_cs().disasm(raw, addr))
            if not decoded or decoded[0].size != 4:
                raise LiftError(f"{addr:#x}: cannot re-decode {raw.hex()}")
            self.by_addr[addr] = (mnemonic, op_str, decoded[0])
            self.raw_by_addr[addr] = raw

    # -- block leaders ----------------------------------------------------
    def leaders(self) -> set[int]:
        code = self.func.code_words
        leaders = {self.func.address}
        for target in self.func.branches:
            if target in code:
                leaders.add(target)
        for _base, arms in self._tables_of_func():
            for arm in arms:
                if arm in code:
                    leaders.add(arm)
        for addr, mnemonic, _op in self.func.indirect_branches:
            if mnemonic != "ldr":  # conditional form falls through
                leaders.add(addr + 4)
        # Conditional branches fall through; anything after an unconditional
        # terminator starts a block.
        for addr in sorted(code):
            _, _, insn = self.by_addr[addr]
            from capstone.arm_const import ARM_INS_B
            if insn.id == ARM_INS_B and _cc(insn) != 14:
                leaders.add(addr + 4)
            if self._is_uncond_terminator(insn) and addr + 4 in code:
                leaders.add(addr + 4)
        # Call fallthrough only matters when targeted; branches cover that.
        return leaders

    def _tables_of_func(self):
        for base, _count in self.func.jump_tables:
            yield base, self.ctx.table_arms[base]

    def _is_uncond_terminator(self, insn) -> bool:
        from capstone.arm_const import (
            ARM_INS_B, ARM_INS_BL, ARM_INS_BLX, ARM_INS_BX,
        )
        if _cc(insn) != 14:
            return False
        # Calls (BL/BLX) always fall through; only B/BX end the block here
        # (pc-writing loads are terminators too; handled via indirects).
        if insn.id in (ARM_INS_B, ARM_INS_BX):
            return True
        if insn.id in (ARM_INS_BL, ARM_INS_BLX):
            return False
        return self._writes_pc(insn)

    @staticmethod
    def _writes_pc(insn) -> bool:
        from capstone.arm import ARM_OP_REG
        for op in insn.operands:
            if op.type != ARM_OP_REG:
                continue
            try:
                if _reg_no(_cs(), op.reg) == 15:
                    return True
            except LiftError:
                continue  # VFP / system register: never the program counter
        return False

    # -- main loop ----------------------------------------------------------
    def emit(self) -> str:
        leaders = self.leaders()
        self.lines.append(f"void {self.ctx.cname[self.func.address]}(CPU *cpu) {{")
        self.lines.append(_TEMPS)
        # Blocks are laid out in address order, but execution must start at
        # the function entry (shared tails can live below it).
        self.lines.append(f"    goto L_{self.func.address:x};")
        for addr in sorted(self.func.code_words):
            if addr in leaders:
                self.lines.append(f"L_{addr:x}:;")
            mnemonic, op_str, insn = self.by_addr[addr]
            try:
                body = self.emit_insn(addr, insn)
            except LiftError as exc:
                raise LiftError(f"{addr:#x} {mnemonic} {op_str}: {exc}") from None
            cc = _cc(insn)
            if cc != 14 and body:
                self.lines.append(f"    if (cc_pass(cpu->cpsr, {cc})) {{")
                self.lines.extend("    " + line for line in body)
                self.lines.append("    }")
            else:
                self.lines.extend("    " + line for line in body)
        self.lines.append("}")
        return "\n".join(self.lines) + "\n"

    # -- per-instruction dispatch -------------------------------------------
    def emit_insn(self, addr: int, insn) -> list[str]:
        from capstone.arm_const import (
            ARM_INS_ADC, ARM_INS_ADD, ARM_INS_ADR, ARM_INS_AND, ARM_INS_ASR,
            ARM_INS_B, ARM_INS_BIC, ARM_INS_BL, ARM_INS_BLX, ARM_INS_BX,
            ARM_INS_CLZ, ARM_INS_CMN, ARM_INS_CMP, ARM_INS_EOR, ARM_INS_LSL,
            ARM_INS_LSR, ARM_INS_MLA, ARM_INS_MOV, ARM_INS_MUL, ARM_INS_MVN,
            ARM_INS_ORR, ARM_INS_ROR, ARM_INS_RRX, ARM_INS_RSB, ARM_INS_RSC,
            ARM_INS_SBC, ARM_INS_SMULL, ARM_INS_SUB, ARM_INS_TEQ, ARM_INS_TST,
            ARM_INS_UMULL,
        )
        alu = {
            ARM_INS_ADD, ARM_INS_ADC, ARM_INS_SUB, ARM_INS_SBC, ARM_INS_RSB,
            ARM_INS_RSC, ARM_INS_AND, ARM_INS_ORR, ARM_INS_EOR, ARM_INS_BIC,
            ARM_INS_MOV, ARM_INS_MVN, ARM_INS_ADR, ARM_INS_CMP, ARM_INS_CMN,
            ARM_INS_TST, ARM_INS_TEQ, ARM_INS_MUL, ARM_INS_MLA, ARM_INS_UMULL,
            ARM_INS_SMULL, ARM_INS_LSL, ARM_INS_LSR, ARM_INS_ASR,
            ARM_INS_ROR, ARM_INS_RRX, ARM_INS_CLZ,
        }
        from capstone.arm_const import (
            ARM_INS_LDM, ARM_INS_LDMDA, ARM_INS_LDMDB, ARM_INS_LDMIB,
            ARM_INS_LDR, ARM_INS_LDRB, ARM_INS_LDRH, ARM_INS_LDRSB,
            ARM_INS_LDRSH, ARM_INS_POP, ARM_INS_PUSH, ARM_INS_SMULBB,
            ARM_INS_STM, ARM_INS_STMDA, ARM_INS_STMDB, ARM_INS_STMIB,
            ARM_INS_STR, ARM_INS_STRB, ARM_INS_STRH, ARM_INS_SXTAH,
            ARM_INS_SXTB, ARM_INS_SXTH, ARM_INS_UXTAH, ARM_INS_UXTB,
            ARM_INS_UXTH, ARM_INS_VLDR, ARM_INS_VSTR,
        )
        if insn.id in alu:
            return self.emit_alu(addr, insn)
        if insn.id in (ARM_INS_LDR, ARM_INS_STR, ARM_INS_LDRB, ARM_INS_STRB,
                        ARM_INS_LDRH, ARM_INS_STRH, ARM_INS_LDRSB,
                        ARM_INS_LDRSH, ARM_INS_VLDR, ARM_INS_VSTR):
            return self.emit_mem(addr, insn)
        if insn.id in (ARM_INS_LDM, ARM_INS_STM, ARM_INS_POP, ARM_INS_PUSH,
                        ARM_INS_LDMIB, ARM_INS_STMIB, ARM_INS_LDMDA,
                        ARM_INS_STMDA, ARM_INS_LDMDB, ARM_INS_STMDB):
            _mnemonic, op_str, _raw = self.by_addr[addr]
            return self.emit_multi(addr, insn, op_str)
        if insn.id in (ARM_INS_UXTB, ARM_INS_UXTH, ARM_INS_SXTB,
                        ARM_INS_SXTH, ARM_INS_UXTAH, ARM_INS_SXTAH,
                        ARM_INS_SMULBB):
            return self.emit_extend(addr, insn)
        from capstone.arm_const import (
            ARM_INS_FMSTAT, ARM_INS_VABS, ARM_INS_VADD, ARM_INS_VCMP,
            ARM_INS_VCMPE, ARM_INS_VCVT, ARM_INS_VDIV, ARM_INS_VLDMIA,
            ARM_INS_VMLA, ARM_INS_VMLS, ARM_INS_VMOV, ARM_INS_VMRS,
            ARM_INS_VMSR, ARM_INS_VMUL, ARM_INS_VNEG, ARM_INS_VNMLA,
            ARM_INS_VNMLS, ARM_INS_VNMUL, ARM_INS_VPOP, ARM_INS_VPUSH,
            ARM_INS_VSQRT, ARM_INS_VSTMIA, ARM_INS_VSUB,
        )
        if insn.id in (ARM_INS_VPUSH, ARM_INS_VPOP):
            return self.emit_vfp_pushpop(addr, insn)
        if insn.id in (ARM_INS_VLDMIA, ARM_INS_VSTMIA):
            return self.emit_vfp_multi(addr, insn)
        if insn.id == ARM_INS_VMOV:
            return self.emit_vmov(addr, insn)
        if insn.id in (ARM_INS_VCMP, ARM_INS_VCMPE):
            return self.emit_vcmp(addr, insn)
        if insn.id == ARM_INS_FMSTAT:
            return self.emit_fmstat(addr, insn)
        if insn.id in (ARM_INS_VMRS, ARM_INS_VMSR):
            raise LiftError(f"VFP system transfer {insn.mnemonic} (game never uses it)")
        if insn.id == ARM_INS_VCVT:
            return self.emit_vcvt(addr, insn)
        if insn.id in (ARM_INS_VADD, ARM_INS_VSUB, ARM_INS_VMUL,
                        ARM_INS_VDIV, ARM_INS_VMLA, ARM_INS_VMLS,
                        ARM_INS_VNMLA, ARM_INS_VNMLS, ARM_INS_VNMUL,
                        ARM_INS_VSQRT, ARM_INS_VNEG, ARM_INS_VABS):
            return self.emit_vfp_arith(addr, insn)
        if insn.id == ARM_INS_B:
            return self.emit_b(addr, insn)
        if insn.id in (ARM_INS_BL, ARM_INS_BLX):
            return self.emit_call(addr, insn)
        if insn.id == ARM_INS_BX:
            return self.emit_bx(addr, insn)
        raise LiftError(f"unhandled id {insn.id} ({insn.mnemonic})")

    # -- operands -------------------------------------------------------------
    def reg(self, reg_id: int, addr: int) -> str:
        """C expression reading an integer register (pc folds to addr+8)."""
        no = _reg_no(_cs(), reg_id)
        if no == 15:
            return f"((uint32_t){addr + 8:#x})"
        return f"cpu->r[{no}]"

    def shift_expr(self, insn, addr: int, value: str, shift, carry_out: str | None) -> str:
        """Barrel-shifter operand; optionally captures the shifter carry."""
        from capstone.arm import ARM_SFT_ASR, ARM_SFT_LSL, ARM_SFT_LSR
        from capstone.arm import ARM_SFT_ROR, ARM_SFT_RRX, ARM_SFT_INVALID
        from capstone.arm import (
            ARM_SFT_ASR_REG, ARM_SFT_LSL_REG, ARM_SFT_LSR_REG, ARM_SFT_ROR_REG,
        )
        stype = shift.type
        if stype == ARM_SFT_INVALID:
            return value
        sval = shift.value
        amount_is_reg = stype in (
            ARM_SFT_ASR_REG, ARM_SFT_LSL_REG, ARM_SFT_LSR_REG, ARM_SFT_ROR_REG)
        if amount_is_reg:
            stype = {
                ARM_SFT_ASR_REG: ARM_SFT_ASR, ARM_SFT_LSL_REG: ARM_SFT_LSL,
                ARM_SFT_LSR_REG: ARM_SFT_LSR, ARM_SFT_ROR_REG: ARM_SFT_ROR,
            }[stype]
        if stype == ARM_SFT_RRX:
            # RRX: (C:Rm) ror 1
            expr = f"((((cpu->cpsr >> 29) & 1u) << 31) | (({value}) >> 1))"
            if carry_out is not None:
                self._pending_carry = f"(({value}) & 1u)"
            return expr
        if amount_is_reg:
            amt = f"({self.reg(sval, addr)} & 0xFFu)"
        else:
            amt = str(int(sval) & 0xFF)
        if stype == ARM_SFT_LSL:
            if not amount_is_reg and int(sval) == 0:
                return value
            if not amount_is_reg:
                return f"(({value}) << ({amt} & 31u))"
            return f"(({amt} & 0xFFu) >= 32 ? 0u : (({value}) << (({amt}) & 31u)))"
        if stype == ARM_SFT_LSR:
            if not amount_is_reg and int(sval) == 0:
                return "0u"
            if not amount_is_reg:
                return f"(({value}) >> {amt})"
            return f"(({amt} >= 32) ? 0u : (({value}) >> ({amt})))"
        if stype == ARM_SFT_ASR:
            if not amount_is_reg and int(sval) == 0:
                return f"((uint32_t)(((int32_t)({value})) >> 31))"
            if not amount_is_reg:
                return f"((uint32_t)(((int32_t)({value})) >> {amt}))"
            return (f"((uint32_t)(((int32_t)({value})) >> "
                    f"((({amt} >= 32)) ? 31 : ({amt}))))")
        if stype == ARM_SFT_ROR:
            if not amount_is_reg and int(sval) == 0:  # ROR #0 encodes RRX
                return self.shift_expr(insn, addr, value, _RrxShift(), carry_out)
            return (f"((({value}) >> (({amt}) & 31u)) | (({value}) << ((-(int)({amt})) & 31u)))"
                    if amount_is_reg else
                    f"((({value}) >> ({amt} & 31u)) | (({value}) << ((32 - ({amt})) & 31u)))")
        raise LiftError(f"unknown shift type {stype}")

    # -- ALU ------------------------------------------------------------------
    def emit_alu(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_IMM, ARM_OP_REG
        from capstone.arm_const import (
            ARM_INS_ADC, ARM_INS_ADD, ARM_INS_ADR, ARM_INS_AND, ARM_INS_ASR,
            ARM_INS_BIC, ARM_INS_CLZ, ARM_INS_CMN, ARM_INS_CMP, ARM_INS_EOR,
            ARM_INS_LSL, ARM_INS_LSR, ARM_INS_MLA, ARM_INS_MOV, ARM_INS_MUL,
            ARM_INS_MVN, ARM_INS_ORR, ARM_INS_ROR, ARM_INS_RRX, ARM_INS_RSB,
            ARM_INS_RSC, ARM_INS_SBC, ARM_INS_SMULL, ARM_INS_SUB,
            ARM_INS_TEQ, ARM_INS_TST, ARM_INS_UMULL,
        )
        ops = insn.operands
        setflags = _writes_flags(insn)
        if insn.id in (ARM_INS_ADC, ARM_INS_SBC, ARM_INS_RSC):
            # Capstone reports update_flags=True for every ADC/SBC/RSC
            # (they read C); trust the encoding's S bit instead.
            word = int.from_bytes(self.raw_by_addr[addr], "little")
            setflags = bool((word >> 20) & 1)
        lines: list[str] = []

        def dst_reg() -> int:
            return _reg_no(_cs(), ops[0].reg)

        def src_op2(index: int) -> str:
            op = ops[index]
            if op.type == ARM_OP_IMM:
                return f"((uint32_t){int(op.imm) & 0xFFFFFFFF:#x})"
            if op.type == ARM_OP_REG:
                return self.shift_expr(insn, addr, self.reg(op.reg, addr), op.shift, None)
            raise LiftError("unexpected operand-2 kind")

        iid = insn.id
        if iid in (ARM_INS_MOV, ARM_INS_MVN, ARM_INS_LSL, ARM_INS_LSR,
                   ARM_INS_ASR, ARM_INS_ROR, ARM_INS_RRX):
            if iid in (ARM_INS_MOV, ARM_INS_MVN) and len(ops) != 2:
                raise LiftError("mov/mvn with unexpected operand count")
            if len(ops) == 3:
                return self.emit_shift_reg(addr, insn)
            if iid == ARM_INS_RRX:
                return self.emit_rrx(addr, insn)
            d = dst_reg()
            if iid == ARM_INS_MOV and ops[1].type == ARM_OP_REG and ops[1].shift.type == 0 and d == _reg_no(_cs(), ops[1].reg):
                return []  # mov rX, rX: nop (covers ARMv6 `nop`)
            v = src_op2(1)
            if iid == ARM_INS_MVN:
                v = f"(~({v}))"
            elif iid == ARM_INS_LSL and not (ops[1].type == ARM_OP_REG and ops[1].shift.type != 0):
                pass  # plain mov handled above; lsl mnemonic w/ explicit shift below
            if d == 15:
                return self.emit_mov_pc(addr, f"({v})")
            lines.append(f"_t0 = ({v});")
            if setflags:
                # Data-processing with S and a shifted operand also updates C
                # from the shifter; model the common cases exactly.
                lines.append("cpu->cpsr = (cpu->cpsr & ~0xC0000000u) | fl_nz(_t0);")
                self.emit_shifter_carry(addr, insn, ops[1], lines)
            lines.append(f"cpu->r[{d}] = _t0;")
            return lines
        if iid == ARM_INS_ADR:
            d = dst_reg()
            lines.append(f"cpu->r[{d}] = ((uint32_t){addr + 8 + int(ops[1].imm):#x});")
            return lines
        if iid == ARM_INS_CLZ:
            d = dst_reg()
            s = self.reg(ops[1].reg, addr)
            lines.append(f"_t0 = ({s});")
            lines.append("_t1 = _t0 ? __builtin_clz(_t0) : 32;")
            lines.append(f"cpu->r[{d}] = _t1;")
            return lines
        if iid in (ARM_INS_MUL, ARM_INS_MLA):
            rd = _reg_no(_cs(), ops[0].reg)
            # MUL Rd, Rm, Rs | MLA Rd, Rm, Rs, Rn
            rm = self.reg(ops[1].reg, addr)
            rs = self.reg(ops[2].reg, addr)
            lines.append(f"_t0 = (uint32_t)(((uint64_t)({rm}) * (uint64_t)({rs})) & 0xFFFFFFFFu);")
            if iid == ARM_INS_MLA:
                rn = self.reg(ops[3].reg, addr)
                lines.append(f"_t0 = _t0 + ({rn});")
            if setflags:
                lines.append("cpu->cpsr = (cpu->cpsr & ~0xC0000000u) | (fl_nz(_t0) & 0xC0000000u);")
            lines.append(f"cpu->r[{rd}] = _t0;")
            return lines
        if iid in (ARM_INS_UMULL, ARM_INS_SMULL):
            rdlo = _reg_no(_cs(), ops[0].reg)
            rdhi = _reg_no(_cs(), ops[1].reg)
            rm = self.reg(ops[2].reg, addr)
            rs = self.reg(ops[3].reg, addr)
            if iid == ARM_INS_UMULL:
                lines.append(f"_t64 = (uint64_t)({rm}) * (uint64_t)({rs});")
            else:
                lines.append(f"_t64 = (uint64_t)(((int64_t)(int32_t)({rm})) * ((int64_t)(int32_t)({rs})));")
            if setflags:
                lines.append("_t0 = (uint32_t)(_t64 >> 32); _t1 = (uint32_t)_t64;")
                lines.append("cpu->cpsr = (cpu->cpsr & ~0xC0000000u) | (fl_nz(_t0 | _t1) & 0xC0000000u);")
            lines.append(f"cpu->r[{rdlo}] = (uint32_t)_t64;")
            lines.append(f"cpu->r[{rdhi}] = (uint32_t)(_t64 >> 32);")
            return lines

        # Three-operand data processing (second source may be shifted).
        if iid in (ARM_INS_CMP, ARM_INS_CMN, ARM_INS_TST, ARM_INS_TEQ):
            rn = self.reg(ops[0].reg, addr)
            op2 = src_op2(1)
            lines.append(f"_t0 = ({rn}); _t1 = ({op2});")
            if iid == ARM_INS_CMP:
                lines.append("_t2 = _t0 - _t1;")
                lines.append("cpu->cpsr = (cpu->cpsr & ~0xF0000000u) | fl_sub(_t0, _t1, _t2);")
            elif iid == ARM_INS_CMN:
                lines.append("_t2 = _t0 + _t1;")
                lines.append("cpu->cpsr = (cpu->cpsr & ~0xF0000000u) | fl_add(_t0, _t1, _t2);")
            elif iid == ARM_INS_TST:
                lines.append("_t2 = _t0 & _t1;")
                lines.append("cpu->cpsr = (cpu->cpsr & ~0xC0000000u) | fl_nz(_t2);")
                self.emit_shifter_carry(addr, insn, ops[1], lines)
            else:
                lines.append("_t2 = _t0 ^ _t1;")
                lines.append("cpu->cpsr = (cpu->cpsr & ~0xC0000000u) | fl_nz(_t2);")
                self.emit_shifter_carry(addr, insn, ops[1], lines)
            return lines

        d = dst_reg()
        rn = self.reg(ops[1].reg, addr)
        op2 = src_op2(2)
        lines.append(f"_t0 = ({rn}); _t1 = ({op2});")
        if iid == ARM_INS_ADD:
            lines.append("_t2 = _t0 + _t1;")
            flag = "fl_add(_t0, _t1, _t2)"
        elif iid == ARM_INS_ADC:
            lines.append("_t2 = _t0 + _t1 + ((cpu->cpsr >> 29) & 1u);")
            flag = "fl_add(_t0 + ((cpu->cpsr >> 29) & 1u), _t1, _t2)"
        elif iid == ARM_INS_SUB:
            lines.append("_t2 = _t0 - _t1;")
            flag = "fl_sub(_t0, _t1, _t2)"
        elif iid == ARM_INS_SBC:
            lines.append("_t2 = _t0 - _t1 - (1u - ((cpu->cpsr >> 29) & 1u));")
            flag = "fl_sub(_t0, _t1 + (1u - ((cpu->cpsr >> 29) & 1u)), _t2)"
        elif iid == ARM_INS_RSB:
            lines.append("_t2 = _t1 - _t0;")
            flag = "fl_sub(_t1, _t0, _t2)"
        elif iid == ARM_INS_RSC:
            lines.append("_t2 = _t1 - _t0 - (1u - ((cpu->cpsr >> 29) & 1u));")
            flag = "fl_sub(_t1, _t0 + (1u - ((cpu->cpsr >> 29) & 1u)), _t2)"
        elif iid == ARM_INS_AND:
            lines.append("_t2 = _t0 & _t1;")
            flag = "fl_nz(_t2)"
        elif iid == ARM_INS_ORR:
            lines.append("_t2 = _t0 | _t1;")
            flag = "fl_nz(_t2)"
        elif iid == ARM_INS_EOR:
            lines.append("_t2 = _t0 ^ _t1;")
            flag = "fl_nz(_t2)"
        elif iid == ARM_INS_BIC:
            lines.append("_t2 = _t0 & ~_t1;")
            flag = "fl_nz(_t2)"
        else:
            raise LiftError("alu fallthrough")
        if d == 15:
            if setflags:
                raise LiftError("pc-destination with S-bit (exception return?)")
            return self.emit_mov_pc(addr, "_t2")
        if setflags:
            if iid in (ARM_INS_AND, ARM_INS_ORR, ARM_INS_EOR, ARM_INS_BIC,
                        ARM_INS_MOV, ARM_INS_MVN):
                lines.append("cpu->cpsr = (cpu->cpsr & ~0xC0000000u) | fl_nz(_t2);")
                self.emit_shifter_carry(addr, insn, ops[2], lines)
            else:
                lines.append(f"cpu->cpsr = (cpu->cpsr & ~0xF0000000u) | {flag};")
        lines.append(f"cpu->r[{d}] = _t2;")
        return lines

    def emit_shifter_carry(self, addr, insn, op, lines: list[str]) -> None:
        """Update the C flag from a data-processing shifter when exact."""
        from capstone.arm import ARM_OP_IMM, ARM_OP_REG, ARM_SFT_INVALID
        if op.type == ARM_OP_IMM:
            # Rotated data-processing immediate with rotate != 0 sets C to
            # bit 31 of the shifter operand; rotate == 0 leaves C alone.
            word = int.from_bytes(self.raw_by_addr[addr], "little")
            if ((word >> 8) & 0xF) != 0:
                if (int(op.imm) >> 31) & 1:
                    lines.append("cpu->cpsr |= 0x20000000u;")
                else:
                    lines.append("cpu->cpsr &= ~0x20000000u;")
            return
        if op.type != ARM_OP_REG or op.shift.type == ARM_SFT_INVALID:
            return
        stype = op.shift.type
        from capstone.arm import (
            ARM_SFT_ASR, ARM_SFT_LSL, ARM_SFT_LSR, ARM_SFT_ROR, ARM_SFT_RRX,
            ARM_SFT_ASR_REG, ARM_SFT_LSL_REG, ARM_SFT_LSR_REG,
            ARM_SFT_ROR_REG,
        )
        is_reg = stype in (
            ARM_SFT_ASR_REG, ARM_SFT_LSL_REG, ARM_SFT_LSR_REG, ARM_SFT_ROR_REG)
        if is_reg:
            stype = {
                ARM_SFT_ASR_REG: ARM_SFT_ASR, ARM_SFT_LSL_REG: ARM_SFT_LSL,
                ARM_SFT_LSR_REG: ARM_SFT_LSR, ARM_SFT_ROR_REG: ARM_SFT_ROR,
            }[stype]
            sval = 0
        else:
            sval = int(op.shift.value)
        rm = self.reg(op.reg, addr)
        if stype == ARM_SFT_RRX or (stype == ARM_SFT_ROR and not is_reg and sval == 0):
            lines.append("cpu->cpsr = (cpu->cpsr & ~0x20000000u) | (((_t0_unused, 0)));")
            lines.pop()
            lines.append(f"cpu->cpsr = (cpu->cpsr & ~0x20000000u) | (((({rm}) & 1u)) << 29);")
        elif not is_reg and sval != 0:
            if stype == ARM_SFT_LSL:
                lines.append(f"cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((({rm}) >> (32 - {sval})) & 1u)) << 29);")
            elif stype in (ARM_SFT_LSR, ARM_SFT_ASR):
                n = 32 if sval == 0 else sval
                lines.append(f"cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((({rm}) >> ({n} - 1)) & 1u)) << 29);")
            elif stype == ARM_SFT_ROR:
                lines.append(f"cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((({rm}) >> (({sval} - 1) & 31)) & 1u)) << 29);")
        elif is_reg:
            amt = f"({self.reg(op.shift.value, addr)} & 0xFFu)"
            lines.append(f"_t3 = {amt};")
            if stype == ARM_SFT_LSL:
                lines.append(f"if (_t3 && _t3 < 32) cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((({rm}) >> (32 - _t3)) & 1u)) << 29);")
                lines.append(f"else if (_t3 == 32) cpu->cpsr = (cpu->cpsr & ~0x20000000u) | (((({rm}) & 1u)) << 29);")
                lines.append("else if (_t3 > 32) cpu->cpsr &= ~0x20000000u;")
            elif stype == ARM_SFT_LSR:
                lines.append(f"if (_t3 && _t3 <= 32) cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((({rm}) >> (_t3 - 1)) & 1u)) << 29);")
                lines.append("else if (_t3 > 32) cpu->cpsr &= ~0x20000000u;")
            elif stype == ARM_SFT_ASR:
                lines.append(f"if (_t3 && _t3 < 32) cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((({rm}) >> (_t3 - 1)) & 1u)) << 29);")
                lines.append(f"else if (_t3 >= 32) cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((({rm}) >> 31) & 1u)) << 29);")
            elif stype == ARM_SFT_ROR:
                lines.append(f"if (_t3 & 31) cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((({rm}) >> ((_t3 - 1) & 31)) & 1u)) << 29);")
                lines.append(f"else if (_t3) cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((({rm}) >> 31) & 1u)) << 29);")

    # -- control flow -----------------------------------------------------------
    def emit_b(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_IMM
        (op,) = insn.operands
        if op.type != ARM_OP_IMM:
            raise LiftError("non-immediate B")
        target = int(op.imm)
        if target in self.func.code_words:
            return [f"goto L_{target:x};"]
        return self.emit_tail_to(addr, target)

    def emit_tail_to(self, addr: int, target: int) -> list[str]:
        if target in self.ctx.cname:
            return [f"{self.ctx.cname[target]}(cpu);", "return;"]
        if target in self.ctx.import_of_stub:
            shim = _shim_name(self.ctx.import_of_stub[target])
            return [f"{shim}(cpu);", "return;"]
        raise LiftError(f"tail branch to unknown {target:#x}")

    def emit_call(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_IMM, ARM_OP_REG
        from capstone.arm_const import ARM_INS_BLX
        (op,) = insn.operands
        ret = f"((uint32_t){(addr | 0x80000000):#x})"
        if op.type == ARM_OP_IMM:
            target = int(op.imm)
            if insn.id == ARM_INS_BLX and target & 1:
                raise LiftError("BLX to Thumb")
            target &= ~1
            if target in self.ctx.cname:
                return [f"cpu->r[14] = {ret};",
                        f"{self.ctx.cname[target]}(cpu);"]
            if target in self.ctx.import_of_stub:
                shim = _shim_name(self.ctx.import_of_stub[target])
                return [f"cpu->r[14] = {ret};",
                        f"{shim}(cpu);"]
            raise LiftError(f"call to unknown {target:#x}")
        if op.type == ARM_OP_REG:
            no = _reg_no(_cs(), op.reg)
            if no == 15:
                raise LiftError("blx pc (unpredictable)")
            return [f"cpu->r[14] = {ret};",
                    f"tdispatch(cpu, cpu->r[{no}]);"]
        raise LiftError("unexpected call operand")

    def emit_bx(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_REG
        (op,) = insn.operands
        if op.type != ARM_OP_REG:
            raise LiftError("non-register BX")
        no = _reg_no(_cs(), op.reg)
        if no == 14:
            return ["return;"]
        return [f"_t0 = cpu->r[{no}];",
                "if ((_t0 & VRET_BIT) && vret_site_ok(_t0 & ~VRET_BIT)) return;",
                "tdispatch(cpu, _t0);",
                "return;"]

    def emit_mov_pc(self, addr: int, value_expr: str) -> list[str]:
        return [f"_t0 = ({value_expr});",
                "if ((_t0 & VRET_BIT) && vret_site_ok(_t0 & ~VRET_BIT)) return;",
                "tdispatch(cpu, _t0);",
                "return;"]


    # -- register-specified shift aliases (lsl Rd, Rm, Rs etc.) -----------------
    def emit_shift_reg(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_REG
        from capstone.arm_const import (
            ARM_INS_ASR, ARM_INS_LSL, ARM_INS_LSR, ARM_INS_ROR,
        )
        ops = insn.operands
        if len(ops) != 3 or any(o.type != ARM_OP_REG for o in ops):
            raise LiftError("shift-alias with unexpected operands")
        if insn.id not in (ARM_INS_LSL, ARM_INS_LSR, ARM_INS_ASR, ARM_INS_ROR):
            raise LiftError("shift-alias with unexpected id")
        d = _reg_no(_cs(), ops[0].reg)
        rm = self.reg(ops[1].reg, addr)
        rs = self.reg(ops[2].reg, addr)
        lines = [f"_t3 = (({rs}) & 0xFFu);", f"_t0 = ({rm});"]
        iid = insn.id
        if iid == ARM_INS_LSL:
            lines.append("_t1 = (_t3 >= 32) ? 0u : (_t0 << _t3);")
            carry = [f"if (_t3 && _t3 < 32) cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((_t0 >> (32 - _t3)) & 1u)) << 29);",
                     f"else if (_t3 == 32) cpu->cpsr = (cpu->cpsr & ~0x20000000u) | (((_t0 & 1u)) << 29);",
                     "else if (_t3 > 32) cpu->cpsr &= ~0x20000000u;"]
        elif iid == ARM_INS_LSR:
            lines.append("_t1 = (_t3 == 0) ? _t0 : (_t3 >= 32 ? 0u : (_t0 >> _t3));")
            carry = ["if (_t3 && _t3 <= 32) cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((_t0 >> (_t3 - 1)) & 1u)) << 29);",
                     "else if (_t3 > 32) cpu->cpsr &= ~0x20000000u;"]
        elif iid == ARM_INS_ASR:
            lines.append("_t1 = (_t3 == 0) ? _t0 : (uint32_t)(((int32_t)_t0) >> (_t3 >= 32 ? 31 : _t3));")
            carry = ["if (_t3 && _t3 < 32) cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((_t0 >> (_t3 - 1)) & 1u)) << 29);",
                     "else if (_t3 >= 32) cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((_t0 >> 31) & 1u)) << 29);"]
        else:
            lines.append("_t1 = (_t3 & 31) ? ((_t0 >> (_t3 & 31)) | (_t0 << ((32 - (_t3 & 31)) & 31))) : _t0;")
            carry = ["if (_t3 & 31) cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((_t0 >> ((_t3 - 1) & 31)) & 1u)) << 29);",
                     "else if (_t3) cpu->cpsr = (cpu->cpsr & ~0x20000000u) | ((((_t0 >> 31) & 1u)) << 29);"]
        if d == 15:
            raise LiftError("shift-alias into pc")
        if _writes_flags(insn):
            lines.append("cpu->cpsr = (cpu->cpsr & ~0xC0000000u) | fl_nz(_t1);")
            lines.extend(carry)
        lines.append(f"cpu->r[{d}] = _t1;")
        return lines

    def emit_rrx(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_REG
        ops = insn.operands
        if len(ops) != 2 or any(o.type != ARM_OP_REG for o in ops):
            raise LiftError("rrx with unexpected operands")
        d = _reg_no(_cs(), ops[0].reg)
        rm = self.reg(ops[1].reg, addr)
        if d == 15:
            raise LiftError("rrx into pc")
        lines = [f"_t0 = ({rm});",
                 "_t1 = ((((cpu->cpsr >> 29) & 1u) << 31) | (_t0 >> 1));"]
        if _writes_flags(insn):
            lines.append("cpu->cpsr = (cpu->cpsr & ~0xC0000000u) | fl_nz(_t1) | (((_t0 & 1u)) << 29);")
        lines.append(f"cpu->r[{d}] = _t1;")
        return lines

    # -- single data transfer ---------------------------------------------------
    def mem_operands(self, addr: int, insn):
        """Decode (dest_kind, base_expr, base_no|None, offset_expr, mode).

        mode is (kind, writeback_expr|None) with kind in pre/post.
        """
        from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG
        ops = insn.operands
        memop = next((operand for operand in ops[1:] if operand.type == ARM_OP_MEM), None)
        if memop is None:
            raise LiftError(
                "memory transfer has no ARM_OP_MEM detail operand; "
                "install capstone >= 5.0.6 or report the instruction detail"
            )
        mem = memop.mem
        if mem.base == 0:
            raise LiftError("memory operand without base")
        base_no = _reg_no(_cs(), mem.base)
        base = f"((uint32_t){addr + 8:#x})" if base_no == 15 else f"cpu->r[{base_no}]"
        if mem.lshift != 0:
            raise LiftError("mem-index lshift field set")
        if mem.index != 0:
            idx = self.reg(mem.index, addr)
            off = self.shift_expr(insn, addr, f"({idx})", memop.shift, None)
            if getattr(memop, "subtracted", False):
                off = f"(0u - ({off}))"
        else:
            off = f"((int32_t){int(mem.disp):d})"
        wb = _is_writeback(insn)
        extra = [o for o in ops[1:] if o.type != ARM_OP_MEM]
        if not wb:
            if extra:
                raise LiftError("post-index shape without writeback flag")
            return base, (None if base_no == 15 else base_no), off, ("pre", None)
        if not extra:
            if base_no == 15:
                raise LiftError("writeback onto pc")
            return base, base_no, off, ("pre", "ea")
        if len(extra) != 1:
            raise LiftError("multi-operand post-index")
        post = extra[0]
        if post.type == ARM_OP_IMM:
            delta = int(post.imm)
            if getattr(post, "subtracted", False):
                delta = -delta
            return base, (None if base_no == 15 else base_no), "0", ("post", str(delta))
        raise LiftError("register post-index")

    def emit_mem(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_REG
        from capstone.arm_const import (
            ARM_INS_LDR, ARM_INS_LDRB, ARM_INS_LDRH, ARM_INS_LDRSB,
            ARM_INS_LDRSH, ARM_INS_STR, ARM_INS_STRB, ARM_INS_STRH,
            ARM_INS_VLDR, ARM_INS_VSTR,
        )
        ops = insn.operands
        if ops[0].type != ARM_OP_REG:
            raise LiftError("non-register transfer destination")
        try:
            dest = _reg_no(_cs(), ops[0].reg)
            dest_is_int = True
        except LiftError:
            dest_is_int = False
        iid = insn.id
        is_load = iid in (ARM_INS_LDR, ARM_INS_LDRB, ARM_INS_LDRH,
                           ARM_INS_LDRSB, ARM_INS_LDRSH, ARM_INS_VLDR)
        base, base_no, off, (mode, wb_expr) = self.mem_operands(addr, insn)
        lines: list[str] = []
        if mode == "pre":
            lines.append(f"_t2 = (uint32_t)((int32_t)({base}) + ({off}));")
        else:
            lines.append(f"_t2 = ({base});")
        if is_load and dest_is_int and dest == base_no and wb_expr is not None:
            raise LiftError("Rt==Rn writeback")
        if not is_load and base_no is None:
            raise LiftError("store with pc base")
        access = {ARM_INS_LDR: "rd32", ARM_INS_STR: "rd32",
                  ARM_INS_LDRB: "rd8", ARM_INS_STRB: "rd8",
                  ARM_INS_LDRH: "rd16", ARM_INS_STRH: "rd16",
                  ARM_INS_LDRSB: "rd8", ARM_INS_LDRSH: "rd16"}.get(iid)
        if iid in (ARM_INS_VLDR, ARM_INS_VSTR):
            kind, no = _vfp_no(_cs(), ops[0].reg)
            if kind == "s":
                if iid == ARM_INS_VLDR:
                    lines.append(f"sset(cpu, {no}, rd32(_t2));")
                else:
                    lines.append(f"wr32(_t2, sget(cpu, {no}));")
            else:
                if no >= 16:
                    raise LiftError("VFP bank d16+ (NEON-only)")
                if iid == ARM_INS_VLDR:
                    lines.append(f"cpu->d[{no}] = rd64(_t2);")
                else:
                    lines.append(f"wr64(_t2, cpu->d[{no}]);")
        elif is_load:
            if dest == 15:
                return self.emit_ldr_pc(addr, insn, lines)
            lines.append(f"_t0 = {access}(_t2);")
            if iid == ARM_INS_LDRSB:
                lines.append("_t0 = (uint32_t)(int32_t)(int8_t)_t0;")
            elif iid == ARM_INS_LDRSH:
                lines.append("_t0 = (uint32_t)(int32_t)(int16_t)_t0;")
            lines.append(f"cpu->r[{dest}] = _t0;")
        else:
            src = self.reg(ops[0].reg, addr)
            write = {"rd32": "wr32", "rd16": "wr16", "rd8": "wr8"}[access]
            lines.append(f"_t0 = ({src});")
            lines.append(f"{write}(_t2, _t0);")
        if wb_expr == "ea":
            lines.append(f"cpu->r[{base_no}] = _t2;")
        elif wb_expr is not None:
            lines.append(f"cpu->r[{base_no}] = _t2 + ({wb_expr});")
        return lines

    def prev_is_mov_lr_pc(self, addr: int) -> bool:
        from capstone.arm import ARM_OP_REG
        from capstone.arm_const import ARM_INS_MOV
        prev = self.by_addr.get(addr - 4)
        if prev is None:
            return False
        _mnemonic, _op_str, insn = prev
        if insn.id != ARM_INS_MOV or len(insn.operands) != 2:
            return False
        try:
            return (_reg_no(_cs(), insn.operands[0].reg) == 14
                    and _reg_no(_cs(), insn.operands[1].reg) == 15)
        except LiftError:
            return False

    def emit_ldr_pc(self, addr: int, insn, prefix: list[str]) -> list[str]:
        from capstone.arm import ARM_OP_MEM
        memop = next((operand for operand in insn.operands[1:] if operand.type == ARM_OP_MEM), None)
        if memop is None:
            raise LiftError(
                "ldr pc has no ARM_OP_MEM detail operand; "
                "install capstone >= 5.0.6 or report the instruction detail"
            )
        mem = memop.mem
        base_no = _reg_no(_cs(), mem.base)
        if base_no == 15 and mem.index != 0:
            return self.emit_dispatch(addr, insn, mem.index)
        if base_no == 15:
            target = int(self.ctx.image.read_u32(addr + 8 + int(mem.disp)))
            if target in self.func.code_words:
                return [f"goto L_{target:x};"]
            return self.emit_tail_to(addr, target)
        lines = list(prefix)
        if self.prev_is_mov_lr_pc(addr):
            ret = f"((uint32_t){(addr | 0x80000000):#x})"
            lines.append(f"cpu->r[14] = {ret};")
            lines.append("tdispatch(cpu, rd32(_t2));")
            return lines
        lines.append("_t0 = rd32(_t2);")
        lines.append("if ((_t0 & VRET_BIT) && vret_site_ok(_t0 & ~VRET_BIT)) return;")
        lines.append("tdispatch(cpu, _t0);")
        lines.append("return;")
        return lines

    def emit_dispatch(self, addr: int, insn, index_reg: int) -> list[str]:
        try:
            arms = self.ctx.table_arms[addr + 8]
        except KeyError:
            raise LiftError("pc-dispatch without a decoded table") from None
        no = _reg_no(_cs(), index_reg)
        lines = [f"_t0 = cpu->r[{no}];", "switch (_t0) {"]
        for index, arm in enumerate(arms):
            if arm not in self.func.code_words:
                raise LiftError(f"table arm {arm:#x} outside function")
            lines.append(f"case {index}: goto L_{arm:x};")
        nxt = addr + 4
        if nxt in self.func.code_words:
            lines.append(f"default: goto L_{nxt:x};")
        else:
            lines.append("default: trabort();")
        lines.append("}")
        return lines

    # -- block transfer -----------------------------------------------------------
    def emit_multi(self, addr: int, insn, op_str: str) -> list[str]:
        from capstone.arm import ARM_OP_REG
        from capstone.arm_const import ARM_INS_LDM, ARM_INS_POP
        raw = self.raw_by_addr[addr]
        word = int.from_bytes(raw, "little")
        if "^" in op_str:
            raise LiftError("user-mode (^) block transfer")
        # The L bit (20) is authoritative for every block-transfer
        # encoding, including the LDMDA/LDMDB/LDMIB aliases.
        is_load = bool((word >> 20) & 1)
        regs = [o for o in insn.operands if o.type == ARM_OP_REG]
        from capstone.arm_const import (
            ARM_INS_LDMDA, ARM_INS_LDMDB, ARM_INS_LDMIB, ARM_INS_STM,
            ARM_INS_STMDA, ARM_INS_STMDB, ARM_INS_STMIB,
        )
        # LDM/STM carry the base first; POP/PUSH are span-only (base = sp).
        has_base = insn.id in (ARM_INS_LDM, ARM_INS_STM, ARM_INS_LDMIB,
                               ARM_INS_STMIB, ARM_INS_LDMDA, ARM_INS_STMDA,
                               ARM_INS_LDMDB, ARM_INS_STMDB)
        if has_base:
            base_no = _reg_no(_cs(), regs[0].reg)
            rlist = [_reg_no(_cs(), o.reg) for o in regs[1:]]
        else:
            base_no = 13
            rlist = [_reg_no(_cs(), o.reg) for o in regs]
        if base_no == 15:
            raise LiftError("block transfer with pc base")
        if not rlist:
            raise LiftError("empty register list")
        if not is_load and 15 in rlist:
            raise LiftError("stm with pc in list")
        if sorted(rlist) != rlist:
            raise LiftError("non-ascending register list")
        p_bit = (word >> 24) & 1
        u_bit = (word >> 23) & 1
        w_bit = (word >> 21) & 1
        n = len(rlist)
        if u_bit == 1 and p_bit == 0:
            start, end = "Rn", f"Rn + {4 * n}"
        elif u_bit == 1 and p_bit == 1:
            start, end = "Rn + 4", f"Rn + {4 * n}"
        elif u_bit == 0 and p_bit == 1:
            start, end = f"Rn - {4 * n}", f"Rn - {4 * n}"
        else:
            start, end = f"Rn - {4 * n} + 4", f"Rn - {4 * n}"
        lines = [f"_t2 = cpu->r[{base_no}];"]
        rn = "_t2"
        start = start.replace("Rn", rn)
        end = end.replace("Rn", rn)
        if is_load:
            for k, reg in enumerate(rlist):
                if reg == 15:
                    continue
                lines.append(f"cpu->r[{reg}] = rd32(({start}) + {4 * k});")
            if w_bit and base_no in rlist:
                raise LiftError("ldm writeback with base in list")
            if w_bit:
                lines.append(f"cpu->r[{base_no}] = ({end});")
            if 15 in rlist:
                lines.append(f"_t0 = rd32(({start}) + {4 * (n - 1)});")
                lines.append("if ((_t0 & VRET_BIT) && vret_site_ok(_t0 & ~VRET_BIT)) return;")
                lines.append("tdispatch(cpu, _t0);")
                lines.append("return;")
        else:
            for k, reg in enumerate(rlist):
                lines.append(f"wr32(({start}) + {4 * k}, cpu->r[{reg}]);")
            if w_bit and base_no in rlist:
                raise LiftError("stm writeback with base in list")
            if w_bit:
                lines.append(f"cpu->r[{base_no}] = ({end});")
        return lines

    # -- extension + packed multiply -------------------------------------------------
    def emit_extend(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_REG
        from capstone.arm_const import (
            ARM_INS_SMULBB, ARM_INS_SXTAH, ARM_INS_SXTB, ARM_INS_SXTH,
            ARM_INS_UXTAH, ARM_INS_UXTB, ARM_INS_UXTH,
        )
        ops = insn.operands
        if any(o.type != ARM_OP_REG for o in ops):
            raise LiftError("extend with non-register operands")
        iid = insn.id
        if iid in (ARM_INS_UXTB, ARM_INS_UXTH, ARM_INS_SXTB, ARM_INS_SXTH):
            if len(ops) != 2:
                raise LiftError("extend shape")
            d = _reg_no(_cs(), ops[0].reg)
            src = self.reg(ops[1].reg, addr)
            if ops[1].shift.type != 0:
                raise LiftError("rotated extend")
            if iid == ARM_INS_UXTB:
                lines = [f"cpu->r[{d}] = (({src}) & 0xFFu);"]
            elif iid == ARM_INS_UXTH:
                lines = [f"cpu->r[{d}] = (({src}) & 0xFFFFu);"]
            elif iid == ARM_INS_SXTB:
                lines = [f"cpu->r[{d}] = (uint32_t)(int32_t)(int8_t)({src});"]
            else:
                lines = [f"cpu->r[{d}] = (uint32_t)(int32_t)(int16_t)({src});"]
            return lines
        if iid in (ARM_INS_UXTAH, ARM_INS_SXTAH):
            if len(ops) != 3:
                raise LiftError("extend-add shape")
            d = _reg_no(_cs(), ops[0].reg)
            rn = self.reg(ops[1].reg, addr)
            rm = self.reg(ops[2].reg, addr)
            if iid == ARM_INS_UXTAH:
                return [f"cpu->r[{d}] = (({rn}) + ((({rm}) & 0xFFFFu)));"]
            return [f"cpu->r[{d}] = (({rn}) + ((uint32_t)(int32_t)(int16_t)({rm})));"]
        if iid == ARM_INS_SMULBB:
            if len(ops) != 3:
                raise LiftError("smulbb shape")
            d = _reg_no(_cs(), ops[0].reg)
            rm = self.reg(ops[1].reg, addr)
            rs = self.reg(ops[2].reg, addr)
            return [f"cpu->r[{d}] = (uint32_t)(((int32_t)(int16_t)({rm})) * ((int32_t)(int16_t)({rs})));"]
        raise LiftError("extend fallthrough")

    # -- VFP ------------------------------------------------------------------
    def _vfp_bank(self, kind: str, no: int) -> int:
        if kind == "d" and no >= 16:
            raise LiftError("VFP bank d16+ (NEON-only)")
        if kind == "s" and not 0 <= no < 32:
            raise LiftError(f"bad S register {no}")
        return no

    def emit_vfp_pushpop(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_REG
        from capstone.arm_const import ARM_INS_VPUSH
        ops = insn.operands
        if not ops or any(o.type != ARM_OP_REG for o in ops):
            raise LiftError("VFP push/pop shape")
        kinds = [_vfp_no(_cs(), o.reg) for o in ops]
        if any(k != kinds[0][0] for k, _n in kinds):
            raise LiftError("mixed S/D push/pop list")
        kind = kinds[0][0]
        nos = [self._vfp_bank(k, n) for k, n in kinds]
        unit = 4 if kind == "s" else 8
        total = unit * len(nos)
        lines: list[str] = []
        if insn.id == ARM_INS_VPUSH:
            lines.append(f"cpu->r[13] -= {total}u;")
            for i, no in enumerate(nos):
                if kind == "s":
                    lines.append(f"wr32(cpu->r[13] + {i * unit}u, sget(cpu, {no}));")
                else:
                    lines.append(f"wr64(cpu->r[13] + {i * unit}u, cpu->d[{no}]);")
        else:
            for i, no in enumerate(nos):
                if kind == "s":
                    lines.append(f"sset(cpu, {no}, rd32(cpu->r[13] + {i * unit}u));")
                else:
                    lines.append(f"cpu->d[{no}] = rd64(cpu->r[13] + {i * unit}u);")
            lines.append(f"cpu->r[13] += {total}u;")
        return lines

    def emit_vfp_multi(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_REG
        from capstone.arm_const import ARM_INS_VSTMIA
        ops = insn.operands
        if len(ops) < 2 or any(o.type != ARM_OP_REG for o in ops):
            raise LiftError("VFP multi shape")
        base_no = _reg_no(_cs(), ops[0].reg)
        if base_no == 15:
            raise LiftError("VFP multi with pc base")
        kinds = [_vfp_no(_cs(), o.reg) for o in ops[1:]]
        if any(k != kinds[0][0] for k, _n in kinds):
            raise LiftError("mixed S/D multi list")
        kind = kinds[0][0]
        nos = [self._vfp_bank(k, n) for k, n in kinds]
        unit = 4 if kind == "s" else 8
        total = unit * len(nos)
        lines = ["_t2 = cpu->r[%d];" % base_no]
        if insn.id == ARM_INS_VSTMIA:
            for i, no in enumerate(nos):
                if kind == "s":
                    lines.append(f"wr32(_t2 + {i * unit}u, sget(cpu, {no}));")
                else:
                    lines.append(f"wr64(_t2 + {i * unit}u, cpu->d[{no}]);")
        else:
            for i, no in enumerate(nos):
                if kind == "s":
                    lines.append(f"sset(cpu, {no}, rd32(_t2 + {i * unit}u));")
                else:
                    lines.append(f"cpu->d[{no}] = rd64(_t2 + {i * unit}u);")
        if _is_writeback(insn):
            lines.append(f"cpu->r[{base_no}] = _t2 + {total}u;")
        return lines

    def emit_vmov(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_REG
        ops = insn.operands
        if any(o.type != ARM_OP_REG for o in ops):
            raise LiftError("VMOV immediate form")
        if len(ops) == 2:
            a, b = ops
            try:
                ka, na = _vfp_no(_cs(), a.reg)
                a_vfp: tuple[str, int] | None = (ka, na)
            except LiftError:
                a_vfp = None
            try:
                kb, nb = _vfp_no(_cs(), b.reg)
                b_vfp: tuple[str, int] | None = (kb, nb)
            except LiftError:
                b_vfp = None
            if a_vfp is not None and b_vfp is not None:
                if a_vfp[0] != b_vfp[0]:
                    raise LiftError("VMOV cross-size S/D")
                self._vfp_bank(*a_vfp)
                self._vfp_bank(*b_vfp)
                if a_vfp[0] == "s":
                    return [f"sset(cpu, {a_vfp[1]}, sget(cpu, {b_vfp[1]}));"]
                return [f"cpu->d[{a_vfp[1]}] = cpu->d[{b_vfp[1]}];"]
            if a_vfp is None and b_vfp is not None and b_vfp[0] == "s":
                dest = _reg_no(_cs(), a.reg)
                if dest == 15:
                    raise LiftError("VMOV to pc")
                self._vfp_bank(*b_vfp)
                return [f"cpu->r[{dest}] = sget(cpu, {b_vfp[1]});"]
            if a_vfp is not None and a_vfp[0] == "s" and b_vfp is None:
                self._vfp_bank(*a_vfp)
                return [f"sset(cpu, {a_vfp[1]}, {self.reg(b.reg, addr)});"]
            raise LiftError("VMOV two-operand shape")
        if len(ops) == 3:
            a, b, c = ops
            try:
                kc, nc = _vfp_no(_cs(), c.reg)
                c_vfp: tuple[str, int] | None = (kc, nc)
            except LiftError:
                c_vfp = None
            try:
                ka, na = _vfp_no(_cs(), a.reg)
                a_vfp = (ka, na)
            except LiftError:
                a_vfp = None
            if c_vfp is not None and c_vfp[0] == "d" and a_vfp is None:
                self._vfp_bank(*c_vfp)
                t = _reg_no(_cs(), a.reg)
                t2 = _reg_no(_cs(), b.reg)
                if t == 15 or t2 == 15:
                    raise LiftError("VMOV core pair to pc")
                return [f"cpu->r[{t}] = (uint32_t)cpu->d[{c_vfp[1]}];",
                        f"cpu->r[{t2}] = (uint32_t)(cpu->d[{c_vfp[1]}] >> 32);"]
            if a_vfp is not None and a_vfp[0] == "d" and c_vfp is None:
                self._vfp_bank(*a_vfp)
                return [f"cpu->d[{a_vfp[1]}] = ((uint64_t)({self.reg(c.reg, addr)}) << 32) | ({self.reg(b.reg, addr)});"]
            raise LiftError("VMOV three-operand shape")
        raise LiftError("VMOV operand count")

    def emit_vcmp(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_IMM, ARM_OP_REG
        ops = insn.operands
        if len(ops) != 2 or ops[0].type != ARM_OP_REG:
            raise LiftError("VCMP shape")
        kind, na = _vfp_no(_cs(), ops[0].reg)
        self._vfp_bank(kind, na)
        if ops[1].type == ARM_OP_IMM:
            if int(ops[1].imm) != 0:
                raise LiftError("VCMP nonzero immediate")
            other: str | None = None
        elif ops[1].type == ARM_OP_REG:
            ko, no = _vfp_no(_cs(), ops[1].reg)
            if ko != kind:
                raise LiftError("VCMP cross-size")
            self._vfp_bank(ko, no)
            other = str(no)
        else:
            raise LiftError("VCMP second operand")
        if kind == "s":
            lines = [f"_f0 = u2f(sget(cpu, {na}));"]
            lines.append(f"_f1 = u2f(sget(cpu, {other}));" if other is not None else "_f1 = 0.0f;")
            lines.append("cpu->fpscr = (cpu->fpscr & ~0xF0000000u) | vfp_nzcv_f(_f0, _f1);")
        else:
            lines = [f"_d0 = u2d(cpu->d[{na}]);"]
            lines.append(f"_d1 = u2d(cpu->d[{other}]);" if other is not None else "_d1 = 0.0;")
            lines.append("cpu->fpscr = (cpu->fpscr & ~0xF0000000u) | vfp_nzcv_d(_d0, _d1);")
        return lines

    def emit_fmstat(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_REG
        ops = insn.operands
        if len(ops) != 2 or any(o.type != ARM_OP_REG for o in ops):
            raise LiftError("FMSTAT shape")
        names = [_cs().reg_name(o.reg) for o in ops]
        if names != ["apsr_nzcv", "fpscr"]:
            raise LiftError(f"FMSTAT operands {names}")
        return ["cpu->cpsr = (cpu->cpsr & ~0xF0000000u) | (cpu->fpscr & 0xF0000000u);"]

    _VCVT_DIRS = {
        "f32.s32": ("s", "s"), "s32.f32": ("s", "s"),
        "f32.f64": ("s", "d"), "f64.f32": ("d", "s"),
        "f64.s32": ("d", "s"), "f32.u32": ("s", "s"),
        "f64.u32": ("d", "s"), "s32.f64": ("s", "d"),
        "u32.f32": ("s", "s"), "u32.f64": ("s", "d"),
    }

    def emit_vcvt(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_REG
        ops = insn.operands
        if len(ops) != 2 or any(o.type != ARM_OP_REG for o in ops):
            raise LiftError("VCVT fixed-point form")
        rest = insn.mnemonic[len("vcvt"):]
        if rest.startswith("."):
            dirs = rest[1:]
        else:
            cond, dot, dirs = rest[:2], rest[2:3], rest[3:]
            if dot != "." or cond not in (
                    "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
                    "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"):
                raise LiftError(f"VCVT mnemonic {insn.mnemonic}")
        expect = self._VCVT_DIRS.get(dirs)
        if expect is None:
            raise LiftError(f"VCVT direction {dirs}")
        kd, nd = _vfp_no(_cs(), ops[0].reg)
        ks, ns = _vfp_no(_cs(), ops[1].reg)
        if (kd, ks) != expect:
            raise LiftError(f"VCVT {dirs} operand kinds {(kd, ks)}")
        self._vfp_bank(kd, nd)
        self._vfp_bank(ks, ns)
        dst, src = dirs.split(".")
        if dst == "f32" and src == "s32":
            return [f"sset(cpu, {nd}, f2u((float)(int32_t)sget(cpu, {ns})));"]
        if dst == "f32" and src == "u32":
            return [f"sset(cpu, {nd}, f2u((float)sget(cpu, {ns})));"]
        if dst == "f64" and src == "s32":
            return [f"cpu->d[{nd}] = d2u((double)(int32_t)sget(cpu, {ns}));"]
        if dst == "f64" and src == "u32":
            return [f"cpu->d[{nd}] = d2u((double)sget(cpu, {ns}));"]
        if dst == "f32" and src == "f64":
            return [f"sset(cpu, {nd}, f2u((float)u2d(cpu->d[{ns}])));"]
        if dst == "f64" and src == "f32":
            return [f"cpu->d[{nd}] = d2u((double)u2f(sget(cpu, {ns})));"]
        if dst == "s32" and src == "f32":
            return [f"sset(cpu, {nd}, vcvt_s32_f(u2f(sget(cpu, {ns}))));"]
        if dst == "s32" and src == "f64":
            return [f"sset(cpu, {nd}, vcvt_s32_d(u2d(cpu->d[{ns}])));"]
        if dst == "u32" and src == "f32":
            return [f"sset(cpu, {nd}, vcvt_u32_f(u2f(sget(cpu, {ns}))));"]
        if dst == "u32" and src == "f64":
            return [f"sset(cpu, {nd}, vcvt_u32_d(u2d(cpu->d[{ns}])));"]
        raise LiftError(f"VCVT direction {dirs}")

    def emit_vfp_arith(self, addr: int, insn) -> list[str]:
        from capstone.arm import ARM_OP_REG
        from capstone.arm_const import (
            ARM_INS_VABS, ARM_INS_VADD, ARM_INS_VDIV, ARM_INS_VMLA,
            ARM_INS_VMLS, ARM_INS_VMUL, ARM_INS_VNEG, ARM_INS_VNMLA,
            ARM_INS_VNMLS, ARM_INS_VNMUL, ARM_INS_VSQRT, ARM_INS_VSUB,
        )
        ops = insn.operands
        if not ops or any(o.type != ARM_OP_REG for o in ops):
            raise LiftError("VFP arithmetic shape")
        kinds = [_vfp_no(_cs(), o.reg) for o in ops]
        if any(k != kinds[0][0] for k, _n in kinds):
            raise LiftError("VFP arithmetic cross-size")
        kind = kinds[0][0]
        nos = [self._vfp_bank(k, n) for k, n in kinds]
        iid = insn.id
        if len(nos) == 3:
            if iid not in (ARM_INS_VADD, ARM_INS_VSUB, ARM_INS_VMUL,
                            ARM_INS_VDIV, ARM_INS_VMLA, ARM_INS_VMLS,
                            ARM_INS_VNMLA, ARM_INS_VNMLS, ARM_INS_VNMUL):
                raise LiftError("VFP three-operand id")
            d, n, m = nos
            if kind == "s":
                get = lambda x: f"u2f(sget(cpu, {x}))"  # noqa: E731
                if iid == ARM_INS_VADD:
                    return [f"_f0 = ({get(n)} + {get(m)});",
                            f"sset(cpu, {d}, f2u(_f0));"]
                if iid == ARM_INS_VSUB:
                    return [f"_f0 = ({get(n)} - {get(m)});",
                            f"sset(cpu, {d}, f2u(_f0));"]
                if iid == ARM_INS_VMUL:
                    return [f"_f0 = ({get(n)} * {get(m)});",
                            f"sset(cpu, {d}, f2u(_f0));"]
                if iid == ARM_INS_VDIV:
                    return [f"_f0 = ({get(n)} / {get(m)});",
                            f"sset(cpu, {d}, f2u(_f0));"]
                if iid == ARM_INS_VNMUL:
                    return [f"_f0 = -({get(n)} * {get(m)});",
                            f"sset(cpu, {d}, f2u(_f0));"]
                pre = [f"_f0 = {get(d)};",
                       f"_f1 = ({get(n)} * {get(m)});"]
                if iid == ARM_INS_VMLA:
                    return pre + [f"sset(cpu, {d}, f2u(_f0 + _f1));"]
                if iid == ARM_INS_VMLS:
                    return pre + [f"sset(cpu, {d}, f2u(_f0 - _f1));"]
                if iid == ARM_INS_VNMLA:
                    return pre + [f"sset(cpu, {d}, f2u(_f1 - _f0));"]
                return pre + [f"sset(cpu, {d}, f2u(-(_f0 + _f1)));"]
            get = lambda x: f"u2d(cpu->d[{x}])"  # noqa: E731
            if iid == ARM_INS_VADD:
                return [f"_d0 = ({get(n)} + {get(m)});",
                        f"cpu->d[{d}] = d2u(_d0);"]
            if iid == ARM_INS_VSUB:
                return [f"_d0 = ({get(n)} - {get(m)});",
                        f"cpu->d[{d}] = d2u(_d0);"]
            if iid == ARM_INS_VMUL:
                return [f"_d0 = ({get(n)} * {get(m)});",
                        f"cpu->d[{d}] = d2u(_d0);"]
            if iid == ARM_INS_VDIV:
                return [f"_d0 = ({get(n)} / {get(m)});",
                        f"cpu->d[{d}] = d2u(_d0);"]
            if iid == ARM_INS_VNMUL:
                return [f"_d0 = -({get(n)} * {get(m)});",
                        f"cpu->d[{d}] = d2u(_d0);"]
            pre = [f"_d0 = {get(d)};",
                   f"_d1 = ({get(n)} * {get(m)});"]
            if iid == ARM_INS_VMLA:
                return pre + [f"cpu->d[{d}] = d2u(_d0 + _d1);"]
            if iid == ARM_INS_VMLS:
                return pre + [f"cpu->d[{d}] = d2u(_d0 - _d1);"]
            if iid == ARM_INS_VNMLA:
                return pre + [f"cpu->d[{d}] = d2u(_d1 - _d0);"]
            return pre + [f"cpu->d[{d}] = d2u(-(_d0 + _d1));"]
        if len(nos) == 2:
            d, m = nos
            if iid == ARM_INS_VSQRT:
                if kind == "s":
                    return [f"sset(cpu, {d}, f2u(sqrtf(u2f(sget(cpu, {m})))));"]
                return [f"cpu->d[{d}] = d2u(sqrt(u2d(cpu->d[{m}])));"]
            if iid == ARM_INS_VNEG:
                if kind == "s":
                    return [f"sset(cpu, {d}, (sget(cpu, {m}) ^ 0x80000000u));"]
                return [f"cpu->d[{d}] = (cpu->d[{m}] ^ 0x8000000000000000ull);"]
            if iid == ARM_INS_VABS:
                if kind == "s":
                    return [f"sset(cpu, {d}, (sget(cpu, {m}) & 0x7FFFFFFFu));"]
                return [f"cpu->d[{d}] = (cpu->d[{m}] & 0x7FFFFFFFFFFFFFFFull);"]
            raise LiftError("VFP two-operand id")
        raise LiftError("VFP arithmetic operand count")

class _RrxShift:
    from capstone.arm import ARM_SFT_RRX as _t

    def __init__(self):
        from capstone.arm import ARM_SFT_RRX
        self.type = ARM_SFT_RRX
        self.value = 0


def _shim_name(symbol: str) -> str:
    return "shim_" + sanitize(symbol.lstrip("_"))



def lift_function(ctx: LiftContext, func) -> str:
    return _Emitter(ctx, func).emit()


SKIP_NAMES = {"dyld_stub_binding_helper"}


def lift_all(ctx: LiftContext) -> tuple[dict[int, str], list[str]]:
    """Translate every function; collect all failures instead of failing fast."""
    out: dict[int, str] = {}
    failures: list[str] = []
    for addr in sorted(ctx.functions):
        func = ctx.functions[addr]
        if func.name in SKIP_NAMES or addr in ctx.import_of_stub:
            continue  # loader glue: translated code calls shims directly
        try:
            out[addr] = lift_function(ctx, func)
        except LiftError as exc:
            failures.append(f"{addr:#x} {func.name}: {exc}")
    return out, failures
