"""Mach-O image view used by the reconstruction layer.

The native analyzer (``radek-macho``) performs the bounded binary parsing and
returns offsets, addresses and symbol tables. This module adds the semantic
layer on top: address translation, typed reads, section lookup and stub
resolution. All reads are bounds checked against the slice they belong to.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path

LAZY_SYMBOL_POINTERS = 7
NON_LAZY_SYMBOL_POINTERS = 6
SYMBOL_STUBS = 8


@dataclass
class Section:
    name: str
    segment: str
    address: int
    size: int
    offset: int
    flags: int
    reserved1: int = 0
    reserved2: int = 0

    @property
    def type(self) -> int:
        return self.flags & 0xFF

    def __repr__(self) -> str:  # pragma: no cover - debugging helper
        return f"<Section {self.segment},{self.name} @0x{self.address:x} {self.size}>"


@dataclass
class Symbol:
    name: str
    value: int
    type: int
    section: int
    description: int

    @property
    def external(self) -> bool:
        return bool(self.type & 0x01)

    @property
    def undefined(self) -> bool:
        return self.section == 0 and (self.type & 0x0E) == 0

    @property
    def thumb(self) -> bool:
        return bool(self.description & 0x0008)


@dataclass
class MachOImage:
    """A single Mach-O slice loaded for reconstruction (never executed)."""

    path: Path
    slice: dict
    data: bytes
    sections: list[Section] = field(default_factory=list)
    symbols: list[Symbol] = field(default_factory=list)
    entry: int | None = None
    function_starts: list[int] = field(default_factory=list)
    stubs: dict[int, str] = field(default_factory=dict)
    imports: list[dict] = field(default_factory=list)
    exports: list[dict] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    # --- construction ---------------------------------------------------------

    @property
    def architecture(self) -> str:
        return self.slice.get("architecture", "unknown")

    @property
    def pointer_size(self) -> int:
        return 8 if self.slice.get("bits") == 64 else 4

    @property
    def little_endian(self) -> bool:
        return not self.slice.get("bigEndian")

    @property
    def base_offset(self) -> int:
        return int(self.slice.get("offset", 0))

    def section(self, segment: str, name: str) -> Section | None:
        for item in self.sections:
            if item.segment == segment and item.name == name:
                return item
        return None

    def sections_named(self, name: str) -> list[Section]:
        return [s for s in self.sections if s.name == name]

    # --- address translation --------------------------------------------------

    def addr_to_offset(self, address: int) -> int | None:
        for item in self.sections:
            if item.size and item.address <= address < item.address + item.size and item.offset:
                return item.offset + (address - item.address)
        return None

    def offset_to_addr(self, offset: int) -> int | None:
        for item in self.sections:
            if item.size and item.offset <= offset < item.offset + item.size and item.address:
                return item.address + (offset - item.offset)
        return None

    def read(self, address: int, length: int) -> bytes:
        offset = self.addr_to_offset(address)
        if offset is None or offset + length > len(self.data):
            raise ValueError(f"read outside image: 0x{address:x}")
        return self.data[offset : offset + length]

    def try_read(self, address: int, length: int) -> bytes | None:
        try:
            return self.read(address, length)
        except ValueError:
            return None

    def read_pointer(self, address: int) -> int | None:
        raw = self.try_read(address, self.pointer_size)
        if raw is None:
            return None
        order = "<" + ("Q" if self.pointer_size == 8 else "I")
        return struct.unpack(order, raw)[0]

    def read_uint(self, address: int, width: int = 4) -> int | None:
        raw = self.try_read(address, width)
        if raw is None:
            return None
        return int.from_bytes(raw, "little" if self.little_endian else "big")

    def read_int(self, address: int, width: int = 4) -> int | None:
        raw = self.try_read(address, width)
        if raw is None:
            return None
        return int.from_bytes(raw, "little" if self.little_endian else "big", signed=True)

    def cstring(self, address: int, limit: int = 4096) -> str | None:
        offset = self.addr_to_offset(address)
        if offset is None:
            return None
        end = self.data.find(b"\x00", offset, min(len(self.data), offset + limit))
        if end < 0:
            return None
        return self.data[offset:end].decode("utf-8", "replace")

    def section_bytes(self, section: Section) -> bytes:
        return self.data[section.offset : section.offset + section.size]

    # --- symbols --------------------------------------------------------------

    def symbol_at(self, address: int) -> Symbol | None:
        for symbol in self.symbols:
            if symbol.value == address and symbol.name:
                return symbol
        return None

    def name_of(self, address: int) -> str | None:
        symbol = self.symbol_at(address)
        if symbol:
            return symbol.name
        stub = self.stubs.get(address)
        if stub:
            return stub
        return None

    def nearest_symbol(self, address: int) -> tuple[str, int] | None:
        best: tuple[str, int] | None = None
        for symbol in self.symbols:
            if not symbol.name or symbol.value > address:
                continue
            delta = address - symbol.value
            if best is None or delta < best[1]:
                best = (symbol.name, delta)
        return best

    def target_name(self, address: int) -> str:
        """Human readable name for a call/branch target."""
        direct = self.name_of(address)
        if direct:
            return direct
        near = self.nearest_symbol(address)
        if near and near[1] < 4096:
            return f"{near[0]}+0x{near[1]:x}"
        return f"sub_{address:x}"


def load(path: Path, slice_info: dict, max_bytes: int = 256 * 1024 * 1024) -> MachOImage:
    """Build an image view for one analyzer slice."""
    size = int(slice_info.get("size", 0))
    offset = int(slice_info.get("offset", 0))
    with path.open("rb") as handle:
        handle.seek(offset)
        data = handle.read(min(size, max_bytes)) if size else b""
    image = MachOImage(path=path, slice=slice_info, data=data)
    for segment in slice_info.get("segments", []):
        for raw in segment.get("sections", []):
            image.sections.append(
                Section(
                    name=raw["name"],
                    segment=raw.get("segment") or segment.get("name", ""),
                    address=int(raw["address"]),
                    size=int(raw["size"]),
                    offset=int(raw["offset"]),
                    flags=int(raw.get("flags", 0)),
                    reserved1=int(raw.get("reserved1", 0)),
                    reserved2=int(raw.get("reserved2", 0)),
                )
            )
    for raw in slice_info.get("symbols", []):
        image.symbols.append(
            Symbol(
                name=raw.get("name", ""),
                value=int(raw.get("value", 0)),
                type=int(raw.get("type", 0)),
                section=int(raw.get("section", 0)),
                description=int(raw.get("description", 0)),
            )
        )
    image.imports = list(slice_info.get("imports", []))
    image.exports = list(slice_info.get("exports", []))
    starts = slice_info.get("functionStarts") or {}
    image.function_starts = [int(a) for a in starts.get("addresses", [])]
    if "entryOffset" in slice_info:
        offset_in_slice = int(slice_info["entryOffset"])
        address = image.offset_to_addr(offset_in_slice)
        image.entry = address
    elif "threadEntry" in slice_info:
        image.entry = int(slice_info["threadEntry"].get("programCounter", 0)) or None
    _resolve_stubs(image, slice_info)
    return image


def _resolve_stubs(image: MachOImage, slice_info: dict) -> None:
    """Map stub and pointer slots to external symbol names using the indirect table."""
    indirect = (slice_info.get("dynamicSymbols") or {}).get("indirectSymbols") or []
    if not indirect:
        image.notes.append("no indirect symbol table; stub targets are unresolved")
        return
    for section in image.sections:
        kind = section.type
        if kind == SYMBOL_STUBS:
            stride = section.reserved2 or _stub_stride(image.architecture)
            base = section.reserved1
        elif kind in (LAZY_SYMBOL_POINTERS, NON_LAZY_SYMBOL_POINTERS):
            stride = image.pointer_size
            base = section.reserved1
        else:
            continue
        if not stride:
            continue
        count = section.size // stride
        for index in range(count):
            table_index = base + index
            if table_index >= len(indirect):
                break
            symbol_index = indirect[table_index]
            if symbol_index == 0x80000000 or symbol_index >= len(image.symbols):
                continue
            symbol = image.symbols[symbol_index]
            if symbol.name:
                image.stubs[section.address + index * stride] = symbol.name


def _stub_stride(architecture: str) -> int:
    return 16 if architecture in ("armv7", "armv7s", "armv6", "arm32-unknown") else 12
