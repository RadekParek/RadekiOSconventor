"""Textual LLVM IR emission for the proven closed-integer leaf subset.

This is deliberately much narrower than a general ARM lifter. It consumes the
IR produced by :mod:`radek.ir` (MOV-immediate, MOVK, immediate ADD/SUB, RET),
which has already rejected memory, calls, branches, external state and unknown
instructions. It does not lift arbitrary Mach-O functions or recover original
source code.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from .ir import Op, Program, Unsupported

_SYMBOL = re.compile(r"^[A-Za-z_.$][A-Za-z0-9_.$]*$")


def _integer(value: int, width: int = 32) -> int:
    """Return the signed spelling of a two's-complement LLVM integer constant."""
    mask = (1 << width) - 1
    value &= mask
    sign = 1 << (width - 1)
    return value - (1 << width) if value & sign else value


def emit(program: Program, function_name: str = "radek_lifted") -> str:
    """Emit verified-subset LLVM IR from one closed integer function.

    The input must be the ``Program`` returned by ``radek.ir.lift``. Instructions
    are emitted as ordinary wrapping ``i32`` operations (no ``nsw``/``nuw``
    assumptions), preserving the machine's modulo-2**32 arithmetic.
    """
    if not _SYMBOL.fullmatch(function_name):
        raise Unsupported("invalid LLVM function name")
    if program.output_architecture not in ("arm64", "armv7"):
        raise Unsupported("LLVM target must be arm64 or armv7")
    if len(program.blocks) != 1:
        raise Unsupported("LLVM leaf emitter requires exactly one basic block")

    registers: dict[int, str] = {}
    body: list[str] = []
    value_index = 0
    returned = False
    instructions = program.blocks[0].instructions

    def fresh() -> str:
        nonlocal value_index
        name = f"%v{value_index}"
        value_index += 1
        return name

    def reg(register: int | None) -> str:
        if register is None or register not in registers:
            raise Unsupported("LLVM leaf references an uninitialized register")
        return registers[register]

    for position, instruction in enumerate(instructions):
        if returned:
            raise Unsupported("instructions follow the return in the lifted leaf")
        if instruction.width != 32:
            raise Unsupported("only the proven 32-bit integer subset is supported by LLVM emission")
        if instruction.op == Op.CONST:
            if instruction.dst is None:
                raise Unsupported("constant instruction has no destination register")
            value = _integer(instruction.immediate)
            name = fresh()
            body.append(f"  {name} = add i32 0, {value}")
            registers[instruction.dst] = name
        elif instruction.op == Op.INSERT:
            if instruction.dst is None or instruction.shift not in (0, 16):
                raise Unsupported("MOVK requires a destination and a 0- or 16-bit halfword shift")
            if not 0 <= instruction.immediate <= 0xFFFF:
                raise Unsupported("MOVK immediate does not fit a halfword")
            previous = reg(instruction.dst)
            shift = instruction.shift
            field = 0xFFFF << shift
            keep_mask = _integer(0xFFFFFFFF ^ field)
            inserted = _integer(instruction.immediate << shift)
            mask_name = fresh()
            body.append(f"  {mask_name} = and i32 {previous}, {keep_mask}")
            name = fresh()
            body.append(f"  {name} = or i32 {mask_name}, {inserted}")
            registers[instruction.dst] = name
        elif instruction.op in (Op.ADD, Op.SUB):
            if instruction.dst is None or instruction.src is None:
                raise Unsupported("arithmetic instruction has an incomplete register operand")
            left = reg(instruction.src)
            right = _integer(instruction.immediate)
            name = fresh()
            opcode = "add" if instruction.op == Op.ADD else "sub"
            body.append(f"  {name} = {opcode} i32 {left}, {right}")
            registers[instruction.dst] = name
        elif instruction.op == Op.RETURN:
            if position != len(instructions) - 1:
                raise Unsupported("return must terminate the lifted leaf")
            body.append(f"  ret i32 {reg(0)}")
            returned = True
        else:
            operation = instruction.op.value if isinstance(instruction.op, Op) else str(instruction.op)
            raise Unsupported("LLVM leaf has no verified lowering for " + operation)

    if not returned:
        raise Unsupported("LLVM leaf has no terminating return")

    triple = (
        "aarch64-unknown-linux-android"
        if program.output_architecture == "arm64"
        else "armv7-unknown-linux-androideabi"
    )
    lines = [
        "; Offline Radek LLVM lift: one verified closed-integer entry routine only.",
        "; This is not recovered source and is not a complete Android game port.",
        f'target triple = "{triple}"',
        "",
        f"define i32 @{function_name}() {{",
        "entry:",
        *body,
        "}",
        "",
    ]
    return "\n".join(lines)


def verify(ir: str, assembler: str | None = None, timeout: int = 20) -> dict:
    """Ask ``llvm-as`` to parse/verify textual IR when that tool is installed.

    Verification is optional and is never treated as evidence of a complete
    conversion. The assembler is invoked without a shell and its output is
    discarded in a private temporary directory.
    """
    executable = assembler or shutil.which("llvm-as")
    if not executable:
        return {"status": "UNAVAILABLE", "tool": None, "message": "llvm-as was not found on PATH"}
    with tempfile.TemporaryDirectory(prefix="radek-llvm-") as directory:
        output = Path(directory) / "module.bc"
        try:
            completed = subprocess.run(
                [str(executable), "-o", str(output), "-"],
                input=ir,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            return {"status": "FAILED", "tool": str(executable), "message": str(exc)}
        if completed.returncode != 0 or not output.is_file():
            return {
                "status": "FAILED",
                "tool": str(executable),
                "message": completed.stdout[-4000:],
            }
    return {"status": "VERIFIED", "tool": str(executable), "message": "llvm-as accepted the module"}
