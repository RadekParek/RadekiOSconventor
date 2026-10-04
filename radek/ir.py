"""Offline leaf-function lifting. This module never executes guest instructions.

The IR can describe a broader machine than the current proven backend accepts.
Only the explicitly decoded straight-line integer subset is lowerable today:
MOV-immediate, MOVK, register MOV (a plain zero-extending copy), immediate
ADD/SUB and RET.
"""

from dataclasses import dataclass, field, asdict
from enum import Enum
import struct
import hashlib


class Unsupported(ValueError):
    pass


class Op(str, Enum):
    CONST = "const"
    INSERT = "insert"
    MOV = "mov"
    ADD = "add"
    SUB = "sub"
    RETURN = "return"
    LOAD = "load"
    STORE = "store"
    COMPARE = "compare"
    BRANCH = "branch"
    CALL = "call"
    ADDRESS = "address"
    PUSH = "push"
    POP = "pop"
    ATOMIC = "atomic"


@dataclass
class Instruction:
    op: Op
    address: int
    dst: int | None = None
    src: int | None = None
    immediate: int = 0
    shift: int = 0
    width: int = 32
    writes_flags: bool = False


@dataclass
class Block:
    address: int
    instructions: list[Instruction] = field(default_factory=list)
    successors: list[int] = field(default_factory=list)


@dataclass
class Program:
    architecture: str
    blocks: list[Block]
    machine_code: bytes
    source_size: int
    output_architecture: str = "arm64"
    thumb: bool = False

    @property
    def target_abi(self) -> str:
        return "arm64-v8a" if self.output_architecture == "arm64" else "armeabi-v7a"

    def report(self):
        return {
            "architecture": self.architecture,
            "outputArchitecture": self.output_architecture,
            "targetAbi": self.target_abi,
            "blocks": [asdict(b) for b in self.blocks],
            "sourceBytes": self.source_size,
            "outputBytes": len(self.machine_code),
            "machineCodeSha256": hashlib.sha256(self.machine_code).hexdigest(),
            "sourceThumb": self.thumb,
            "backend": "preserved-arm64" if self.output_architecture == "arm64" and self.architecture == "arm64" else f"offline-{self.architecture}-to-{self.output_architecture}",
        }


def _emit_arm64(i: Instruction) -> bytes:
    if i.op == Op.CONST:
        value = i.immediate & 0xFFFFFFFF
        words = [0x52800000 | ((value & 0xFFFF) << 5) | i.dst]
        if value >> 16:
            words.append(0x72A00000 | ((value >> 16) << 5) | i.dst)
    elif i.op == Op.INSERT:
        words = [0x72800000 | ((i.shift // 16) << 21) | (i.immediate << 5) | i.dst]
    elif i.op == Op.MOV:
        # ORR <W|X>d, WZR/XZR, <W|X>m — a plain register copy.
        base = 0xAA000000 if i.width == 64 else 0x2A000000
        words = [base | (i.src << 16) | (31 << 5) | i.dst]
    elif i.op in (Op.ADD, Op.SUB):
        if not 0 <= i.immediate <= 4095:
            raise Unsupported("ARM immediate cannot be lowered to a single safe ADD/SUB")
        words = [(0x11000000 if i.op == Op.ADD else 0x51000000) | (i.immediate << 10) | (i.src << 5) | i.dst]
    elif i.op == Op.RETURN:
        words = [0xD65F03C0]
    else:
        raise Unsupported("IR operation has no verified backend: " + i.op)
    return b"".join(struct.pack("<I", w) for w in words)


def _armv7_immediate(value: int) -> int | None:
    """Encode the ARM rotated-eight-bit immediate field, if representable."""
    value &= 0xFFFFFFFF
    for rotate in range(16):
        shift = rotate * 2
        for imm8 in range(256):
            rotated = ((imm8 >> shift) | (imm8 << (32 - shift))) & 0xFFFFFFFF if shift else imm8
            if rotated == value:
                return (rotate << 8) | imm8
    return None


def _movw(rd: int, value: int) -> int:
    return 0xE3000000 | ((value & 0xF000) << 4) | (rd << 12) | (value & 0x0FFF)


def _movt(rd: int, value: int) -> int:
    return 0xE3400000 | ((value & 0xF000) << 4) | (rd << 12) | (value & 0x0FFF)


def _emit_armv7(i: Instruction) -> bytes:
    if i.op == Op.CONST:
        value = i.immediate & 0xFFFFFFFF
        words = [_movw(i.dst, value & 0xFFFF)]
        if value >> 16:
            words.append(_movt(i.dst, value >> 16))
    elif i.op == Op.INSERT:
        if i.shift != 16:
            raise Unsupported("ARMv7 backend only lowers a high-half MOVK/MOVT")
        words = [_movt(i.dst, i.immediate & 0xFFFF)]
    elif i.op == Op.MOV:
        if not (0 <= i.src <= 15 and 0 <= i.dst <= 15):
            raise Unsupported("ARMv7 MOV register operand out of range")
        words = [0xE1A00000 | i.src | (i.dst << 12)]
    elif i.op in (Op.ADD, Op.SUB):
        immediate = _armv7_immediate(i.immediate)
        if immediate is None:
            raise Unsupported("immediate cannot be represented by one ARMv7 ADD/SUB")
        base = 0xE2800000 if i.op == Op.ADD else 0xE2400000
        words = [base | (i.src << 16) | (i.dst << 12) | immediate]
    elif i.op == Op.RETURN:
        words = [0xE12FFF1E]  # BX LR
    else:
        raise Unsupported("IR operation has no verified ARMv7 backend: " + i.op.value)
    return b"".join(struct.pack("<I", word) for word in words)


def lift(
    code: bytes,
    architecture: str,
    thumb: bool = False,
    target_arch: str = "arm64",
    target_abi: str | None = None,
) -> Program:
    if architecture not in ("arm64", "armv7", "armv7s", "armv6"):
        raise Unsupported("no safe backend for " + architecture)
    target_aliases = {
        "arm64": "arm64",
        "arm64-v8a": "arm64",
        "armv7": "armv7",
        "armeabi-v7a": "armv7",
    }
    if target_abi is not None:
        abi_arch = target_aliases.get(target_abi)
        if abi_arch is None:
            raise Unsupported("unsupported Android target ABI: " + target_abi)
        requested_arch = target_aliases.get(target_arch)
        if target_arch != "arm64" and requested_arch != abi_arch:
            raise Unsupported("conflicting Android target architecture and ABI")
        target_arch = abi_arch
    else:
        target_arch = target_aliases.get(target_arch)
    if target_arch not in ("arm64", "armv7"):
        raise Unsupported("unsupported Android target architecture")
    instructions, initialized, output = [], set(), bytearray()
    p = 0
    while p < len(code) and len(instructions) < 4096:
        start = p
        if architecture == "arm64":
            if p + 4 > len(code):
                raise Unsupported("truncated ARM64 instruction")
            w = struct.unpack_from("<I", code, p)[0]
            p += 4
            if w == 0xD65F03C0:
                i = Instruction(Op.RETURN, start)
            elif w & 0xFF800000 in (0x52800000, 0x72800000):
                shift = (w >> 21 & 3) * 16
                if shift > 16:
                    raise Unsupported("invalid 32-bit MOV encoding")
                op = Op.CONST if w & 0xFF800000 == 0x52800000 else Op.INSERT
                imm = w >> 5 & 0xFFFF
                i = Instruction(
                    op, start, w & 31, immediate=imm << shift if op == Op.CONST else imm, shift=shift
                )
            elif w & 0xFFC00000 in (0x11000000, 0x51000000):
                i = Instruction(
                    Op.ADD if w >> 30 & 1 == 0 else Op.SUB, start, w & 31, w >> 5 & 31, w >> 10 & 4095
                )
            elif w & 0x7FE00000 == 0x2A000000 and w & 0xFC00 == 0 and w & 0x3E0 == 0x3E0:
                # ORR <W|X>d, WZR/XZR, <W|X>m with no shift: a plain register
                # copy (the canonical MOV-register encoding).
                width = 64 if w >> 31 & 1 else 32
                source = w >> 16 & 31
                if source == 31:
                    # MOV <W|X>d, WZR/XZR zeroes the destination.
                    i = Instruction(Op.CONST, start, w & 31, immediate=0, width=width)
                else:
                    i = Instruction(Op.MOV, start, w & 31, source, width=width)
            else:
                raise Unsupported(f"ARM64 instruction 0x{w:08x} at +0x{start:x} is not in the proven subset")
        elif not thumb:
            if p + 4 > len(code):
                raise Unsupported("truncated ARM instruction")
            w = struct.unpack_from("<I", code, p)[0]
            p += 4
            if w == 0xE12FFF1E:
                i = Instruction(Op.RETURN, start)
            elif w >> 28 == 14 and w & 0x0E000000 == 0x02000000:
                opcode, dst, src = w >> 21 & 15, w >> 12 & 15, w >> 16 & 15
                rot, imm = (w >> 8 & 15) * 2, w & 255
                if rot:
                    imm = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF
                if opcode == 13 and src == 0:
                    i = Instruction(Op.CONST, start, dst, immediate=imm, writes_flags=bool(w & 1 << 20))
                elif opcode in (2, 4):
                    i = Instruction(
                        Op.SUB if opcode == 2 else Op.ADD,
                        start,
                        dst,
                        src,
                        imm,
                        writes_flags=bool(w & 1 << 20),
                    )
                else:
                    raise Unsupported("ARM data processing operation outside proven subset")
            elif w & 0xFFFF0FF0 == 0xE1A00000:
                # MOV Rd, Rm (cond AL, no shift, S=0): a plain register copy.
                source = w & 15
                destination = w >> 12 & 15
                if source == 15:
                    raise Unsupported("MOV from PC is not a closed integer value")
                if destination == 15:
                    raise Unsupported("MOV to PC is control flow, not a closed integer copy")
                i = Instruction(Op.MOV, start, destination, source)
            else:
                raise Unsupported(f"ARM instruction 0x{w:08x} at +0x{start:x} is unsupported")
        else:
            if p + 2 > len(code):
                raise Unsupported("truncated Thumb instruction")
            w = struct.unpack_from("<H", code, p)[0]
            p += 2
            if w == 0x4770:
                i = Instruction(Op.RETURN, start)
            elif w & 0xF800 in (0x2000, 0x3000, 0x3800):
                dst, imm = w >> 8 & 7, w & 255
                op = {0x2000: Op.CONST, 0x3000: Op.ADD, 0x3800: Op.SUB}[w & 0xF800]
                i = Instruction(op, start, dst, dst if op != Op.CONST else None, imm, writes_flags=True)
            elif w & 0xFBF0 in (0xF240, 0xF2C0):
                if architecture == "armv6":
                    raise Unsupported("Thumb-2 MOVW/MOVT is unavailable on ARMv6")
                if p + 2 > len(code):
                    raise Unsupported("truncated Thumb-2 MOVW/MOVT")
                second = struct.unpack_from("<H", code, p)[0]
                p += 2
                if second & 0x8000:
                    raise Unsupported("invalid Thumb-2 MOVW/MOVT")
                imm = ((w & 15) << 12) | ((w >> 10 & 1) << 11) | ((second >> 12 & 7) << 8) | (second & 255)
                top = w & 0xFBF0 == 0xF2C0
                i = Instruction(
                    Op.INSERT if top else Op.CONST,
                    start,
                    second >> 8 & 15,
                    immediate=imm,
                    shift=16 if top else 0,
                )
            elif w & 0xFF00 == 0x4600:
                # MOV Rd, Rm (T2 register form, including high registers).
                source = w >> 3 & 15
                destination = (w & 0x80) >> 4 | w & 7
                if source in (13, 15):
                    raise Unsupported("MOV from SP/PC is not a closed integer value")
                if destination == 15:
                    raise Unsupported("MOV to PC is control flow, not a closed integer copy")
                i = Instruction(Op.MOV, start, destination, source)
            else:
                raise Unsupported(
                    f"Thumb instruction 0x{w:04x} at +0x{start:x} is unsupported (branches/IT require a future CFG backend)"
                )
        # Never leak guest writes to registers the Android JNI caller expects preserved.
        # ARMv7 AAPCS keeps r4-r11 and reserves r9; only r0-r3/r12 are volatile.
        allowed_destinations = (
            {0, 1, 2, 3, 12}
            if target_arch == "armv7"
            else set(range(13 if architecture != "arm64" else 16))
        )
        if i.dst is not None and i.dst not in allowed_destinations:
            raise Unsupported("special/platform/callee-saved register write")
        if i.op == Op.INSERT and i.dst not in initialized:
            raise Unsupported("read of uninitialized register")
        if i.src is not None and i.src not in initialized:
            raise Unsupported("input/stack register dependency is not a closed leaf function")
        if i.op == Op.RETURN and 0 not in initialized:
            raise Unsupported("return value is not initialized")
        if i.dst is not None:
            initialized.add(i.dst)
        instructions.append(i)
        if architecture == "arm64" and target_arch == "arm64":
            output.extend(code[start:p])
        elif target_arch == "armv7":
            output.extend(_emit_armv7(i))
        else:
            output.extend(_emit_arm64(i))
        if i.op == Op.RETURN:
            return Program(architecture, [Block(0, instructions)], bytes(output), p, target_arch, thumb)
    raise Unsupported("entry point does not terminate within 4096 verified instructions")
