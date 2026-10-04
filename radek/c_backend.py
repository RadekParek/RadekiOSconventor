"""Portable C lowering for the verified closed-integer leaf IR.

This backend consumes only ``Program`` objects returned by ``radek.ir.lift``.
It does not implement calls, memory, control-flow graphs, Apple runtime objects,
or game lifecycle behavior. Unsigned 32-bit operations preserve ARM W-register
wraparound without invoking signed-overflow undefined behavior in C.
"""

from __future__ import annotations

import re

from .ir import Op, Program, Unsupported

_SYMBOL = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def emit(program: Program, function_name: str = "radek_translated_entry") -> str:
    """Emit one standalone ``uint32_t(void)`` C function from a proven leaf."""
    if not _SYMBOL.fullmatch(function_name):
        raise Unsupported("invalid C function name")
    if program.output_architecture not in ("arm64", "armv7"):
        raise Unsupported("C backend target must be arm64 or armv7")
    if len(program.blocks) != 1:
        raise Unsupported("C leaf backend requires exactly one basic block")

    instructions = program.blocks[0].instructions
    returned = False
    body: list[str] = []
    initialized: set[int] = set()
    register_limit = 16 if program.output_architecture == "arm64" else 13

    def register(value: int | None) -> int:
        if value is None or not 0 <= value < register_limit:
            raise Unsupported("C leaf contains a special, callee-saved, or invalid register operand")
        return value

    for position, instruction in enumerate(instructions):
        if returned:
            raise Unsupported("instructions follow the return in the lifted leaf")
        if instruction.width != 32:
            raise Unsupported("C leaf backend only supports 32-bit integer instructions")
        if instruction.op == Op.CONST:
            dst = register(instruction.dst)
            immediate = instruction.immediate & 0xFFFFFFFF
            body.append(f"    r[{dst}] = UINT32_C(0x{immediate:08x});")
            initialized.add(dst)
        elif instruction.op == Op.INSERT:
            dst = register(instruction.dst)
            if dst not in initialized:
                raise Unsupported("C MOVK reads an uninitialized register")
            if instruction.shift not in (0, 16) or not 0 <= instruction.immediate <= 0xFFFF:
                raise Unsupported("C MOVK requires a valid halfword shift and immediate")
            mask = (0xFFFF << instruction.shift) & 0xFFFFFFFF
            inserted = (instruction.immediate << instruction.shift) & 0xFFFFFFFF
            body.append(
                f"    r[{dst}] = (r[{dst}] & ~UINT32_C(0x{mask:08x})) | "
                f"UINT32_C(0x{inserted:08x});"
            )
            initialized.add(dst)
        elif instruction.op in (Op.ADD, Op.SUB):
            dst = register(instruction.dst)
            src = register(instruction.src)
            if src not in initialized:
                raise Unsupported("C arithmetic reads an uninitialized register")
            immediate = instruction.immediate & 0xFFFFFFFF
            operator = "+" if instruction.op == Op.ADD else "-"
            body.append(
                f"    r[{dst}] = (uint32_t)(r[{src}] {operator} UINT32_C(0x{immediate:08x}));"
            )
            initialized.add(dst)
        elif instruction.op == Op.RETURN:
            if position != len(instructions) - 1:
                raise Unsupported("return must terminate the lifted leaf")
            if 0 not in initialized:
                raise Unsupported("C leaf returns an uninitialized register")
            body.append("    return r[0];")
            returned = True
        else:
            operation = instruction.op.value if isinstance(instruction.op, Op) else str(instruction.op)
            raise Unsupported("C leaf has no verified lowering for " + operation)

    if not returned:
        raise Unsupported("C leaf has no terminating return")

    return "\n".join(
        [
            "/* Generated from a statically proven closed-integer ARM leaf. */",
            "/* This function is not a complete game port or an APK entry point. */",
            "#include <stdint.h>",
            "",
            "#ifdef __cplusplus",
            'extern "C" {',
            "#endif",
            f"uint32_t {function_name}(void) {{",
            "    uint32_t r[32] = {0};",
            *body,
            "}",
            "#ifdef __cplusplus",
            "}",
            "#endif",
            "",
        ]
    )
