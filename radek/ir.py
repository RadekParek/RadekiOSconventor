"""Offline leaf-function lifting. This module never executes guest instructions.

The IR can describe a broader machine than the current proven backend accepts.
Only the explicitly decoded straight-line integer subset is lowerable today.
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

    def report(self):
        return {
            "architecture": self.architecture,
            "blocks": [asdict(b) for b in self.blocks],
            "sourceBytes": self.source_size,
            "outputBytes": len(self.machine_code),
            "machineCodeSha256": hashlib.sha256(self.machine_code).hexdigest(),
            "backend": "preserved-arm64" if self.architecture == "arm64" else "offline-arm32-to-arm64",
        }


def _emit(i: Instruction) -> bytes:
    if i.op == Op.CONST:
        value = i.immediate & 0xFFFFFFFF
        words = [0x52800000 | ((value & 0xFFFF) << 5) | i.dst]
        if value >> 16:
            words.append(0x72A00000 | ((value >> 16) << 5) | i.dst)
    elif i.op == Op.INSERT:
        words = [0x72800000 | ((i.shift // 16) << 21) | (i.immediate << 5) | i.dst]
    elif i.op in (Op.ADD, Op.SUB):
        if not 0 <= i.immediate <= 4095:
            raise Unsupported("ARM immediate cannot be lowered to a single safe ADD/SUB")
        words = [(0x11000000 if i.op == Op.ADD else 0x51000000) | (i.immediate << 10) | (i.src << 5) | i.dst]
    elif i.op == Op.RETURN:
        words = [0xD65F03C0]
    else:
        raise Unsupported("IR operation has no verified backend: " + i.op)
    return b"".join(struct.pack("<I", w) for w in words)


def lift(code: bytes, architecture: str, thumb: bool = False) -> Program:
    if architecture not in ("arm64", "armv7", "armv7s", "armv6"):
        raise Unsupported("no safe backend for " + architecture)
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
            else:
                raise Unsupported(
                    f"Thumb instruction 0x{w:04x} at +0x{start:x} is unsupported (branches/IT require a future CFG backend)"
                )
        # ARM32 SP/LR/PC and ARM64 platform/callee-saved registers never cross the ABI.
        limit = 16 if architecture == "arm64" else 13
        if i.dst is not None and not 0 <= i.dst < limit:
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
        output.extend(code[start:p] if architecture == "arm64" else _emit(i))
        if i.op == Op.RETURN:
            return Program(architecture, [Block(0, instructions)], bytes(output), p)
    raise Unsupported("entry point does not terminate within 4096 verified instructions")
