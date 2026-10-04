"""Objective-C metadata recovery from a Mach-O image.

Classes, categories, protocols, selectors, ivars and properties are read from
the runtime sections. Both the classic ("big") and the iOS 14+ relative
("small") method list encodings are supported; the representation is chosen from
the list flags and then validated, and anything that does not validate is
reported as unresolved rather than guessed.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from .image import MachOImage

FAST_DATA_MASK_64 = 0x00007FFFFFFFFFF8
FAST_DATA_MASK_32 = 0xFFFFFFFC
SMALL_METHOD_LIST = 0x80000000


@dataclass
class ObjCMethod:
    selector: str
    types: str | None
    implementation: int | None
    kind: str  # instance | class | optional-instance | optional-class
    owner: str

    def report(self) -> dict:
        return {
            "selector": self.selector,
            "kind": self.kind,
            "types": self.types,
            "implementation": f"0x{self.implementation:x}" if self.implementation else None,
        }


@dataclass
class ObjCProperty:
    name: str
    attributes: str | None


@dataclass
class ObjCIvar:
    name: str
    type: str | None
    offset: int | None
    size: int


@dataclass
class ObjCProtocol:
    name: str
    address: int
    methods: list[ObjCMethod] = field(default_factory=list)
    properties: list[ObjCProperty] = field(default_factory=list)
    inherited: list[str] = field(default_factory=list)


@dataclass
class ObjCClass:
    name: str
    address: int
    superclass: str | None
    metaclass: int | None
    swift: bool
    methods: list[ObjCMethod] = field(default_factory=list)
    ivars: list[ObjCIvar] = field(default_factory=list)
    properties: list[ObjCProperty] = field(default_factory=list)
    protocols: list[str] = field(default_factory=list)

    def report(self) -> dict:
        return {
            "name": self.name,
            "address": f"0x{self.address:x}",
            "superclass": self.superclass,
            "swift": self.swift,
            "methods": [m.report() for m in self.methods[:200]],
            "methodCount": len(self.methods),
            "ivars": [{"name": i.name, "type": i.type, "size": i.size} for i in self.ivars[:100]],
            "properties": [{"name": p.name, "attributes": p.attributes} for p in self.properties[:100]],
            "protocols": self.protocols,
        }


@dataclass
class ObjCCategory:
    name: str
    target: str | None
    methods: list[ObjCMethod] = field(default_factory=list)
    properties: list[ObjCProperty] = field(default_factory=list)
    protocols: list[str] = field(default_factory=list)


@dataclass
class ObjCRuntime:
    classes: list[ObjCClass] = field(default_factory=list)
    categories: list[ObjCCategory] = field(default_factory=list)
    protocols: list[ObjCProtocol] = field(default_factory=list)
    selectors: list[str] = field(default_factory=list)
    message_selectors: list[str] = field(default_factory=list)
    message_references: list[dict] = field(default_factory=list)
    class_references: list[str] = field(default_factory=list)
    super_references: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    @property
    def present(self) -> bool:
        return bool(
            self.classes
            or self.categories
            or self.protocols
            or self.selectors
            or self.message_references
            or self.class_references
            or self.super_references
        )

    def report(self) -> dict:
        return {
            "classCount": len(self.classes),
            "categoryCount": len(self.categories),
            "protocolCount": len(self.protocols),
            "selectorCount": len(self.selectors),
            "messageSelectorCount": len(self.message_selectors),
            "classes": [c.report() for c in self.classes[:200]],
            "categories": [
                {
                    "name": c.name,
                    "target": c.target,
                    "methods": [m.report() for m in c.methods[:100]],
                    "protocols": c.protocols,
                }
                for c in self.categories[:100]
            ],
            "protocols": [
                {
                    "name": p.name,
                    "methods": [m.report() for m in p.methods[:100]],
                    "inherited": p.inherited,
                }
                for p in self.protocols[:100]
            ],
            "selectors": self.selectors[:500],
            "messageSelectors": self.message_selectors[:200],
            "messageReferences": self.message_references[:200],
            "classReferences": self.class_references[:200],
            "notes": self.notes,
        }


# --- list helpers -------------------------------------------------------------


def _list_header(image: MachOImage, address: int) -> tuple[int, int, int] | None:
    raw = image.try_read(address, 8)
    if raw is None:
        return None
    entsize_and_flags, count = struct.unpack("<II" if image.little_endian else ">II", raw)
    return entsize_and_flags, count, address + 8


def _relative(image: MachOImage, address: int, keep_flag: bool = False) -> int | tuple[bool, int] | None:
    """Resolve a signed 32-bit relative pointer stored at ``address``.

    With ``keep_flag`` the result is ``(low_bit_set, target)``; the low bit of a
    relative selector offset marks an indirect reference through a pointer slot.
    """
    raw = image.try_read(address, 4)
    if raw is None:
        return None
    delta = struct.unpack("<i" if image.little_endian else ">i", raw)[0]
    target = address + (delta & ~1 if keep_flag else delta)
    return (bool(delta & 1), target) if keep_flag else target


def _plausible_selector(name: str | None) -> bool:
    if not name or len(name) > 256:
        return False
    return all(ch.isprintable() for ch in name)


def selector_from_reference(image: MachOImage, address: int | None) -> str | None:
    """Resolve a selector string or a pointer slot such as an ``__objc_selrefs`` entry.

    ``__objc_msgrefs`` commonly points at the selector-reference slot rather
    than at the selector's C string. Try the supplied address and at most two
    bounded pointer dereferences; never scan arbitrary image data for a guessed
    name.
    """
    current = address
    for _ in range(3):
        if not current:
            return None
        candidate = image.cstring(current)
        if _plausible_selector(candidate):
            return candidate
        current = image.read_pointer(current)
    return None


def _methods(image: MachOImage, address: int, owner: str, kind: str) -> list[ObjCMethod]:
    header = _list_header(image, address)
    if header is None:
        return []
    entsize_and_flags, count, start = header
    if count > 65535 or count == 0:
        return []
    use_relative = bool(entsize_and_flags & SMALL_METHOD_LIST)
    stride = 12 if use_relative else image.pointer_size * 3
    declared = entsize_and_flags & 0xFFFC
    if declared and declared != stride:
        # Trust the declared entry size only when it matches a known layout.
        if declared not in (12, 24, 36):
            return []
        stride = declared
    if count * stride > 16 * 1024 * 1024:
        return []
    methods: list[ObjCMethod] = []
    for i in range(count):
        entry = start + i * stride
        if use_relative:
            name_addr = _relative(image, entry, keep_flag=True)
            types_addr = _relative(image, entry + 4)
            imp = _relative(image, entry + 8)
            selector = None
            if name_addr is not None:
                indirect, direct = name_addr
                # A relative selector is either a direct pointer to the name or,
                # when the low bit of the offset is set, a pointer to a slot in
                # __objc_selrefs holding the selector.
                if indirect:
                    slot = image.read_pointer(direct)
                    selector = image.cstring(slot) if slot else None
                if not selector:
                    candidate = image.cstring(direct)
                    if _plausible_selector(candidate):
                        selector = candidate
                    else:
                        slot = image.read_pointer(direct)
                        candidate = image.cstring(slot) if slot else None
                        if _plausible_selector(candidate):
                            selector = candidate
        else:
            name_addr = image.read_pointer(entry)
            types_addr = image.read_pointer(entry + image.pointer_size)
            imp = image.read_pointer(entry + image.pointer_size * 2)
            selector = image.cstring(name_addr) if name_addr else None
        if not selector:
            continue
        methods.append(
            ObjCMethod(
                selector=selector,
                types=image.cstring(types_addr) if types_addr else None,
                implementation=imp,
                kind=kind,
                owner=owner,
            )
        )
    return methods


def _properties(image: MachOImage, address: int) -> list[ObjCProperty]:
    header = _list_header(image, address)
    if header is None:
        return []
    _flags, count, start = header
    if not count or count > 65535:
        return []
    stride = image.pointer_size * 2
    result = []
    for i in range(count):
        entry = start + i * stride
        name = image.read_pointer(entry)
        attributes = image.read_pointer(entry + image.pointer_size)
        if not name:
            continue
        result.append(
            ObjCProperty(name=image.cstring(name) or "", attributes=image.cstring(attributes) if attributes else None)
        )
    return result


def _protocols(image: MachOImage, address: int) -> list[str]:
    count = image.read_uint(address, image.pointer_size) if address else None
    if not count or count > 4096:
        return []
    names = []
    for i in range(count):
        pointer = image.read_pointer(address + image.pointer_size * (1 + i))
        if not pointer:
            continue
        name = _protocol_name(image, pointer)
        if name:
            names.append(name)
    return names


def _protocol_name(image: MachOImage, address: int) -> str | None:
    pointer = image.read_pointer(address + image.pointer_size)
    if not pointer:
        return None
    return image.cstring(pointer)


def _ivars(image: MachOImage, address: int) -> list[ObjCIvar]:
    header = _list_header(image, address)
    if header is None:
        return []
    _flags, count, start = header
    if not count or count > 4096:
        return []
    stride = 4 * image.pointer_size + 8  # offset, name, type, alignment, size
    result = []
    for i in range(count):
        entry = start + i * stride
        offset_pointer = image.read_pointer(entry)
        name_pointer = image.read_pointer(entry + image.pointer_size)
        type_pointer = image.read_pointer(entry + image.pointer_size * 2)
        size = image.read_uint(entry + image.pointer_size * 3 + 4) or 0
        result.append(
            ObjCIvar(
                name=image.cstring(name_pointer) or "?",
                type=image.cstring(type_pointer),
                offset=image.read_uint(offset_pointer) if offset_pointer else None,
                size=size,
            )
        )
    return result


# --- main entry points --------------------------------------------------------


def _class_ro(image: MachOImage, address: int) -> dict | None:
    wide = image.pointer_size == 8
    bits = image.read_pointer(address + (32 if wide else 16))
    if not bits:
        return None
    data = bits & (FAST_DATA_MASK_64 if wide else FAST_DATA_MASK_32)
    if not data:
        return None
    if wide:
        name = image.cstring(image.read_pointer(data + 24) or 0) if image.read_pointer(data + 24) else None
        fields = {
            "flags": image.read_uint(data),
            "instanceSize": image.read_uint(data + 8),
            "name": name,
            "methods": image.read_pointer(data + 32),
            "protocols": image.read_pointer(data + 40),
            "ivars": image.read_pointer(data + 48),
            "properties": image.read_pointer(data + 64),
        }
    else:
        name = image.cstring(image.read_pointer(data + 16) or 0) if image.read_pointer(data + 16) else None
        fields = {
            "flags": image.read_uint(data),
            "instanceSize": image.read_uint(data + 8),
            "name": name,
            "methods": image.read_pointer(data + 20),
            "protocols": image.read_pointer(data + 24),
            "ivars": image.read_pointer(data + 28),
            "properties": image.read_pointer(data + 36),
        }
    fields["swift"] = bool(bits & 1)
    fields["data"] = data
    return fields


def recover(image: MachOImage) -> ObjCRuntime:
    """Recover Objective-C metadata from an image."""
    runtime = ObjCRuntime()
    class_list = _first(image, "__objc_classlist")
    cat_list = _first(image, "__objc_catlist")
    proto_list = _first(image, "__objc_protolist")

    if not (
        class_list
        or cat_list
        or proto_list
        or _first(image, "__objc_methname")
        or _first(image, "__objc_selrefs")
        or _first(image, "__objc_msgrefs")
    ):
        return runtime

    # Selectors: every string in __objc_methname plus explicit selector refs.
    selector_names: list[str] = []
    for section in image.sections_named("__objc_methname"):
        data = image.section_bytes(section)
        for part in data.split(b"\x00"):
            if part:
                selector_names.append(part.decode("utf-8", "replace"))
    runtime.selectors = list(dict.fromkeys(selector_names))

    for section in image.sections_named("__objc_selrefs"):
        data = image.section_bytes(section)
        for i in range(0, len(data) - image.pointer_size + 1, image.pointer_size):
            pointer = int.from_bytes(data[i : i + image.pointer_size], "little" if image.little_endian else "big")
            name = selector_from_reference(image, pointer)
            if name and name not in runtime.selectors:
                runtime.selectors.append(name)

    # Message refs: {imp/objc_msgSend pointer, selector reference}. The second
    # field often points to a slot in __objc_selrefs, not directly to a string.
    for section in image.sections_named("__objc_msgrefs"):
        data = image.section_bytes(section)
        stride = image.pointer_size * 2
        for i in range(0, len(data) - stride + 1, stride):
            implementation = int.from_bytes(
                data[i : i + image.pointer_size], "little" if image.little_endian else "big"
            )
            selector_reference = int.from_bytes(
                data[i + image.pointer_size : i + stride], "little" if image.little_endian else "big"
            )
            name = selector_from_reference(image, selector_reference)
            if len(runtime.message_references) < 200:
                runtime.message_references.append(
                    {
                        "address": f"0x{section.address + i:x}",
                        "implementation": f"0x{implementation:x}" if implementation else None,
                        "selectorReference": f"0x{selector_reference:x}" if selector_reference else None,
                        "selector": name,
                    }
                )
            if name and name not in runtime.message_selectors:
                runtime.message_selectors.append(name)

    for name in ("__objc_classrefs", "__objc_superrefs"):
        for section in image.sections_named(name):
            data = image.section_bytes(section)
            for i in range(0, len(data) - image.pointer_size + 1, image.pointer_size):
                pointer = int.from_bytes(
                    data[i : i + image.pointer_size], "little" if image.little_endian else "big"
                )
                if not pointer:
                    continue
                label = _class_name_at(image, pointer)
                if label:
                    bucket = runtime.class_references if name == "__objc_classrefs" else runtime.super_references
                    if label not in bucket:
                        bucket.append(label)

    if proto_list:
        data = image.section_bytes(proto_list)
        for i in range(0, len(data) - image.pointer_size + 1, image.pointer_size):
            pointer = int.from_bytes(
                data[i : i + image.pointer_size], "little" if image.little_endian else "big"
            )
            if not pointer:
                continue
            name = _protocol_name(image, pointer) or "?"
            protocol = ObjCProtocol(name=name, address=pointer)
            wide = image.pointer_size == 8
            offsets = {"instance": 24, "class": 32, "optional-instance": 40, "optional-class": 48}
            for kind, offset in offsets.items():
                address = image.read_pointer(pointer + (offset if wide else offset // 2))
                if address:
                    protocol.methods.extend(_methods(image, address, name, kind))
            inherited = image.read_pointer(pointer + image.pointer_size * 2)
            if inherited:
                protocol.inherited = _protocols(image, inherited)
            properties = image.read_pointer(pointer + (56 if wide else 28))
            if properties:
                protocol.properties = _properties(image, properties)
            runtime.protocols.append(protocol)

    if class_list:
        data = image.section_bytes(class_list)
        for i in range(0, len(data) - image.pointer_size + 1, image.pointer_size):
            pointer = int.from_bytes(
                data[i : i + image.pointer_size], "little" if image.little_endian else "big"
            )
            if not pointer:
                continue
            parsed = _class_ro(image, pointer)
            if not parsed or not parsed["name"]:
                runtime.notes.append(f"class at 0x{pointer:x} has unreadable class_ro_t")
                continue
            name = parsed["name"]
            superclass = _class_name_at(image, image.read_pointer(pointer + image.pointer_size) or 0)
            metaclass = image.read_pointer(pointer)
            item = ObjCClass(
                name=name,
                address=pointer,
                superclass=superclass,
                metaclass=metaclass,
                swift=bool(parsed["swift"]),
            )
            if parsed["methods"]:
                item.methods.extend(_methods(image, parsed["methods"], name, "instance"))
            if parsed["ivars"]:
                item.ivars.extend(_ivars(image, parsed["ivars"]))
            if parsed["properties"]:
                item.properties.extend(_properties(image, parsed["properties"]))
            if parsed["protocols"]:
                item.protocols = _protocols(image, parsed["protocols"])
            if metaclass:
                meta = _class_ro(image, metaclass)
                if meta and meta["methods"]:
                    item.methods.extend(_methods(image, meta["methods"], name, "class"))
            runtime.classes.append(item)

    if cat_list:
        data = image.section_bytes(cat_list)
        for i in range(0, len(data) - image.pointer_size + 1, image.pointer_size):
            pointer = int.from_bytes(
                data[i : i + image.pointer_size], "little" if image.little_endian else "big"
            )
            if not pointer:
                continue
            name = image.cstring(image.read_pointer(pointer) or 0) or "?"
            category = ObjCCategory(name=name, target=None)
            target = image.read_pointer(pointer + image.pointer_size)
            if target:
                category.target = _class_name_at(image, target)
            instance = image.read_pointer(pointer + image.pointer_size * 2)
            klass = image.read_pointer(pointer + image.pointer_size * 3)
            protocols = image.read_pointer(pointer + image.pointer_size * 4)
            properties = image.read_pointer(pointer + image.pointer_size * 5)
            if instance:
                category.methods.extend(_methods(image, instance, name, "instance"))
            if klass:
                category.methods.extend(_methods(image, klass, name, "class"))
            if protocols:
                category.protocols = _protocols(image, protocols)
            if properties:
                category.properties = _properties(image, properties)
            runtime.categories.append(category)

    if not runtime.classes and (class_list or cat_list):
        runtime.notes.append("class list present but no class could be reconstructed")
    return runtime


def _first(image: MachOImage, name: str):
    sections = image.sections_named(name)
    return sections[0] if sections else None


def _class_name_at(image: MachOImage, address: int) -> str | None:
    if not address:
        return None
    parsed = _class_ro(image, address)
    if parsed and parsed["name"]:
        return parsed["name"]
    # Some references point straight at class_ro_t.
    name = image.cstring(image.read_pointer(address + (24 if image.pointer_size == 8 else 16)) or 0)
    return name or None
