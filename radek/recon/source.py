"""Structured reconstruction: register tracking, message sends and pseudocode.

The output is a *reconstruction*, not recovered source. Registers are tracked
only through straight-line, provably local effects (page-relative addressing,
move-immediate pairs, literal loads) and every statement that cannot be proven
keeps an explicit ``?`` marker.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from .disasm import Function, Instr
from .image import MachOImage
from .objc import ObjCRuntime, selector_from_reference

WIDTHS = {8: "uint64_t", 4: "uint32_t", 2: "uint16_t", 1: "uint8_t"}

REPORT_LISTING_LINES = 120


def _trim(listing: list[str]) -> list[str]:
    """Keep the machine-readable report small; the markdown report shows more."""
    if len(listing) <= REPORT_LISTING_LINES:
        return list(listing)
    return list(listing[:REPORT_LISTING_LINES]) + [
        f"/* … {len(listing) - REPORT_LISTING_LINES} further reconstructed statements */"
    ]


@dataclass
class ReconstructedFunction:
    address: int
    name: str
    signature: str
    listing: list[str] = field(default_factory=list)
    calls: list[str] = field(default_factory=list)
    selectors: list[str] = field(default_factory=list)
    strings: list[str] = field(default_factory=list)
    frame_size: int | None = None
    blocks: int = 0
    instructions: int = 0
    unknown: int = 0
    confidence: str = "medium"

    def report(self) -> dict:
        return {
            "name": self.name,
            "address": f"0x{self.address:x}",
            "signature": self.signature,
            "blocks": self.blocks,
            "instructions": self.instructions,
            "unknownInstructions": self.unknown,
            "frameSize": self.frame_size,
            "calls": self.calls[:32],
            "selectors": self.selectors[:32],
            "strings": self.strings[:16],
            "confidence": self.confidence,
            "listing": _trim(self.listing),
        }


class _Tracker:
    """Local register value tracking for one function."""

    def __init__(self, image: MachOImage):
        self.image = image
        self.state: dict[int, tuple[str, int]] = {}

    def update(self, instruction: Instr) -> None:
        if instruction.dst is None:
            return
        if instruction.kind == "adrp" and instruction.immediate is not None:
            self.state[instruction.dst] = ("page", instruction.immediate)
        elif instruction.kind == "move" and instruction.immediate is not None:
            existing = self.state.get(instruction.dst)
            if instruction.mnemonic == "movk" and existing and existing[0] == "const":
                self.state[instruction.dst] = ("const", existing[1] | instruction.immediate)
            elif instruction.mnemonic == "movz":
                self.state[instruction.dst] = ("const", instruction.immediate)
            elif instruction.mnemonic in ("movw", "movt"):
                if instruction.mnemonic == "movt" and existing and existing[0] == "const":
                    self.state[instruction.dst] = ("const", existing[1] | instruction.immediate)
                else:
                    self.state[instruction.dst] = ("const", instruction.immediate)
            else:
                self.state[instruction.dst] = ("const", instruction.immediate)
        elif instruction.kind == "arith" and instruction.immediate is not None:
            if not instruction.mnemonic.startswith(("add", "sub")):
                # Logical/shifted operations do not produce address or constant sums.
                self.state.pop(instruction.dst, None)
                return
            base = self.state.get(instruction.sources[0]) if instruction.sources else None
            if base and base[0] == "page":
                self.state[instruction.dst] = ("address", base[1] + instruction.immediate)
            elif base and base[0] == "const":
                value = base[1] + instruction.immediate if instruction.mnemonic.startswith("add") else base[1] - instruction.immediate
                self.state[instruction.dst] = ("const", value & 0xFFFFFFFFFFFFFFFF)
            else:
                self.state.pop(instruction.dst, None)
        elif instruction.kind == "loadlit" and instruction.target is not None:
            self.state[instruction.dst] = ("load", instruction.target)
        elif instruction.kind == "load":
            base = self.state.get(instruction.sources[0]) if instruction.sources else None
            if base and base[0] in ("address", "page"):
                target = base[1] + (instruction.immediate or 0)
                pointer = self.image.read_pointer(target)
                if pointer:
                    self.state[instruction.dst] = ("ref", pointer)
                else:
                    self.state[instruction.dst] = ("load", target)
            else:
                self.state.pop(instruction.dst, None)
        elif instruction.kind == "move" and instruction.sources:
            source = self.state.get(instruction.sources[0])
            if source:
                self.state[instruction.dst] = source
            else:
                self.state.pop(instruction.dst, None)
        else:
            self.state.pop(instruction.dst, None)

    def address_of(self, register: int) -> int | None:
        value = self.state.get(register)
        if value and value[0] in ("address", "page", "load"):
            return value[1]
        return None

    def constant_of(self, register: int) -> int | None:
        value = self.state.get(register)
        return value[1] if value and value[0] == "const" else None

    def reference_of(self, register: int) -> int | None:
        """Value loaded into a register from a tracked address (a pointer we can name)."""
        value = self.state.get(register)
        return value[1] if value and value[0] == "ref" else None


class Reconstructor:
    def __init__(self, image: MachOImage, objc: ObjCRuntime):
        self.image = image
        self.objc = objc
        self.strings_seen: set[str] = set()
        self.selectors_seen: set[str] = set()

    # --- reference resolution -------------------------------------------------

    def describe_address(self, address: int) -> tuple[str, str | None, str | None]:
        """Return (label, string value, selector) for a referenced address."""
        for section in self.image.sections:
            if not section.size or not (section.address <= address < section.address + section.size):
                continue
            name = section.name
            if name == "__objc_selrefs":
                selector = selector_from_reference(self.image, address)
                return "objc_selrefs", selector, selector
            if name == "__objc_msgrefs":
                stride = self.image.pointer_size * 2
                relative = address - section.address
                slot = relative % stride
                if slot in (0, self.image.pointer_size):
                    entry = address - slot
                    selector_reference = self.image.read_pointer(entry + self.image.pointer_size)
                    selector = selector_from_reference(self.image, selector_reference)
                    return "objc_msgrefs", selector, selector
                return "objc_msgrefs", None, None
            pointer = self.image.read_pointer(address)
            if name == "__objc_classrefs" and pointer:
                return "objc_classrefs", self._class_name(pointer), None
            if name == "__objc_superrefs" and pointer:
                return "objc_superrefs", self._class_name(pointer), None
            if name == "__cstring":
                value = self.image.cstring(address)
                return "cstring", value, None
            if name == "__cfstring":
                return "cfstring", self._cfstring(address), None
            if name == "__objc_methname":
                return "methname", self.image.cstring(address), self.image.cstring(address)
            if name == "__objc_classname":
                return "classname", self.image.cstring(address), None
            if name == "__const" or name == "__data":
                return name, None, None
            return name, None, None
        return "unknown", None, None

    def _class_name(self, pointer: int) -> str | None:
        from .objc import _class_ro

        parsed = _class_ro(self.image, pointer)
        if parsed and parsed["name"]:
            return parsed["name"]
        return self.image.cstring(self.image.read_pointer(pointer + 24) or 0) if pointer else None

    def describe_pointer(self, pointer: int) -> tuple[str, str | None]:
        """Name a pointer that was loaded out of a reference section."""
        for section in self.image.sections:
            if not section.size or not (section.address <= pointer < section.address + section.size):
                continue
            if section.name == "__objc_methname":
                return "selector", self.image.cstring(pointer)
            if section.name == "__objc_selrefs":
                return "selector", selector_from_reference(self.image, pointer)
            if section.name == "__objc_msgrefs":
                _label, _value, selector = self.describe_address(pointer)
                return "selector", selector
            if section.name == "__objc_classname":
                return "class", self.image.cstring(pointer)
            if section.name == "__objc_data":
                return "class", self._class_name(pointer)
            if section.name == "__cstring":
                return "string", self.image.cstring(pointer)
            if section.name == "__cfstring":
                return "string", self._cfstring(pointer)
        return "pointer", None

    def _cfstring(self, address: int) -> str | None:
        # CFStringLiteral: isa, flags, characters pointer, length
        pointer = self.image.read_pointer(address + self.image.pointer_size * 2)
        return self.image.cstring(pointer) if pointer else None

    # --- statement rendering --------------------------------------------------

    def _statement(self, instruction: Instr, tracker: _Tracker, function: Function) -> str:
        prefix = f"    /* 0x{instruction.address:x} */ "
        if instruction.unknown:
            return f"{prefix}/* undecoded instruction 0x{instruction.raw:0{instruction.size * 2}x} */ ?;"
        name = instruction.mnemonic
        operands = instruction.operands
        if instruction.kind == "ret":
            return f"{prefix}return;  // {name} {operands}"
        if instruction.kind == "call":
            callee = "?"
            if instruction.target is not None:
                callee = self.image.target_name(instruction.target)
            if "objc_msgSend" in callee:
                selector = self._selector_argument(tracker, function, instruction)
                receiver = self._receiver_argument(tracker)
                return f"{prefix}[{receiver} {selector}]  // {callee}"
            return f"{prefix}{callee}(...);"
        if instruction.kind == "branch":
            target = f"0x{instruction.target:x}" if instruction.target is not None else operands
            if instruction.condition:
                return f"{prefix}if ({instruction.condition}) goto {target};"
            return f"{prefix}goto {target};"
        if instruction.kind in ("load", "store"):
            width = 8 if instruction.dst is not None or "ldr" in name else 8
            text = f"{prefix}"
            if instruction.kind == "load":
                line = f"{text}{operands.split(',')[0]} = *({WIDTHS[width]} *)({operands.split('[')[-1].rstrip(']')});"
                reference = tracker.reference_of(instruction.dst) if instruction.dst is not None else None
                if reference:
                    label, value = self.describe_pointer(reference)
                    if value:
                        line += f"  // {label}: {value[:64]}"
                return line
            return f"{text}*({WIDTHS[width]} *)({operands.split('[')[-1].rstrip(']')}) = {operands.split(',')[0]};"
        if instruction.kind == "loadlit":
            value = self.describe_address(instruction.target or 0)
            note = f"  // {value[0]}" if value[0] != "unknown" else ""
            if value[1]:
                note += f" {value[1][:64]!r}"
            return f"{prefix}{name} {operands}{note}"
        if instruction.kind in ("adrp", "adr"):
            return f"{prefix}{operands.split(',')[0]} = &{value_label(instruction)};"
        if instruction.kind == "move":
            constant = tracker.constant_of(instruction.dst) if instruction.dst is not None else None
            if constant is not None:
                return f"{prefix}{operands.split(',')[0]} = 0x{constant:x};"
            return f"{prefix}{name} {operands};"
        if instruction.kind == "arith":
            line = f"{prefix}{name} {operands};"
            if instruction.dst is not None:
                address = tracker.address_of(instruction.dst)
                if address is not None:
                    label, _value, selector = self.describe_address(address)
                    line += f"  // -> 0x{address:x}"
                    if label != "unknown":
                        line += f" {label}"
                    if selector:
                        line += f" {selector}"
            return line
        if instruction.kind == "compare":
            return f"{prefix}{name} {operands};  // sets flags"
        if instruction.kind == "select":
            return f"{prefix}{operands.split(',')[0]} = {instruction.condition} ? {operands.split(',')[1].strip()} : {operands.split(',')[2].strip()};"
        if instruction.kind in ("float", "simd"):
            return f"{prefix}{name} {operands};  // floating point / SIMD"
        if instruction.kind == "system":
            return f"{prefix}{name} {operands};"
        if instruction.kind == "trap":
            return f"{prefix}/* system trap: {name} {operands} */"
        return f"{prefix}{name} {operands};"

    def _receiver_argument(self, tracker: _Tracker) -> str:
        reference = tracker.reference_of(0)
        if reference:
            label, name = self.describe_pointer(reference)
            if label == "class" and name:
                return name
            if label == "string" and name:
                return f"@\"{name}\""
        return "receiver /* unresolved */"

    def _selector_argument(self, tracker: _Tracker, function: Function, instruction: Instr) -> str:
        """Best effort selector for an objc_msgSend call at this instruction."""
        reference = tracker.reference_of(1)
        if reference:
            label, name = self.describe_pointer(reference)
            if label == "selector" and name:
                return f"@selector({name})"
        address = tracker.address_of(1)
        if address is not None:
            _label, _value, selector = self.describe_address(address)
            if selector:
                return f"@selector({selector})"
        for send in function.message_sends:
            if abs(int(send.get("from", "0x0"), 16) - instruction.address) < 64:
                return f"@selector({send.get('selector')})"
        return "selector /* unresolved */"

    # --- function reconstruction ----------------------------------------------

    def function(self, function: Function) -> ReconstructedFunction:
        tracker = _Tracker(self.image)
        listing: list[str] = []
        calls: list[str] = []
        selectors: list[str] = []
        strings: list[str] = []
        frame_size: int | None = None
        for instruction in function.instructions:
            if instruction.kind == "arith" and instruction.dst == 31 and instruction.immediate:
                frame_size = max(frame_size or 0, instruction.immediate)
            if instruction.kind == "call" and instruction.target is not None:
                calls.append(self.image.target_name(instruction.target))
            if instruction.kind == "adrp" or instruction.kind == "loadlit":
                tracker.update(instruction)
                address = tracker.address_of(instruction.dst or 0)
                if address is not None:
                    label, value, selector = self.describe_address(address)
                    if selector:
                        selectors.append(selector)
                    if value and label in ("cstring", "cfstring"):
                        strings.append(value)
            else:
                tracker.update(instruction)
            listing.append(self._statement(instruction, tracker, function))
        for send in function.message_sends:
            if send.get("selector"):
                selectors.append(send["selector"])
        confidence = "high" if not function.unknown_instructions and function.complete else "medium"
        if function.unknown_instructions:
            confidence = "low"
        signature = self._signature(function)
        return ReconstructedFunction(
            address=function.address,
            name=function.name,
            signature=signature,
            listing=listing,
            calls=calls,
            selectors=selectors,
            strings=strings,
            frame_size=frame_size,
            blocks=len(function.blocks),
            instructions=len(function.instructions),
            unknown=function.unknown_instructions,
            confidence=confidence,
        )

    def _signature(self, function: Function) -> str:
        arguments = _argument_count(function)
        parameters = ", ".join(f"intptr_t arg{i}" for i in range(arguments)) or "void"
        return f"intptr_t {function.name}({parameters})"


def value_label(instruction: Instr) -> str:
    if instruction.immediate is None:
        return "?"
    return f"data_0x{instruction.immediate:x}"


def _argument_count(function: Function) -> int:
    """Registers x0-x7 read before being written are treated as arguments."""
    used: set[int] = set()
    written: set[int] = set()
    for instruction in function.instructions:
        for register in instruction.sources:
            if register in used or register in written or register > 7:
                continue
            used.add(register)
        if instruction.dst is not None:
            written.add(instruction.dst)
    return len(used)


def call_graph(functions: list[Function], image: MachOImage) -> dict:
    edges: list[dict] = []
    for function in functions:
        for call in function.calls:
            edges.append(
                {
                    "from": function.name,
                    "to": call["name"],
                    "address": call["target"],
                    "external": image.name_of(int(call["target"], 16)) is None
                    or any(s.undefined and s.name == call["name"] for s in image.symbols),
                }
            )
    return {"edgeCount": len(edges), "edges": edges[:4000]}
