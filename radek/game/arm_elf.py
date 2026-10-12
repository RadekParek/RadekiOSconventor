"""Writes an ARM32 ELF shared object that exports machine-code functions.

The output is an ET_DYN ELF32 little-endian EM_ARM file with the pieces the
Android dynamic linker reads: PT_LOAD segments (read/execute for code and
tables, read/write for the dynamic section), PT_DYNAMIC, a DT_HASH table and
the dynamic symbol and string tables. Section headers are omitted; the dynamic
linker does not need them.
"""

from __future__ import annotations

import struct

EM_ARM = 40
ET_DYN = 3
EF_ARM_EABI_VER5 = 0x05000000
PAGE = 0x1000

PT_LOAD = 1
PT_DYNAMIC = 2
PF_X, PF_W, PF_R = 1, 2, 4

DT_NULL = 0
DT_HASH = 4
DT_STRTAB = 5
DT_SYMTAB = 6
DT_STRSZ = 10
DT_SYMENT = 11
DT_SONAME = 14

_TEXT_SECTION = 4  # index of .text in the section table below
STB_GLOBAL = 1
STT_FUNC = 2

_EHDR = struct.Struct("<16sHHIIIIIHHHHHH")
_PHDR = struct.Struct("<IIIIIIII")
_SYM = struct.Struct("<IIIBBH")
_DYN = struct.Struct("<II")
_SHDR = struct.Struct("<IIIIIIIIII")

SHT_PROGBITS = 1
SHT_STRTAB = 3
SHT_HASH = 5
SHT_DYNAMIC = 6
SHT_DYNSYM = 11
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4


class ElfError(ValueError):
    """The requested object cannot be represented."""


def elf_hash(name: bytes) -> int:
    """The SysV ELF symbol hash used by DT_HASH."""
    h = 0
    for byte in name:
        h = ((h << 4) + byte) & 0xFFFFFFFF
        g = h & 0xF0000000
        if g:
            h ^= g >> 24
        h &= ~g & 0xFFFFFFFF
    return h


SHT_NULL_TYPE = 0


def _align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def write_shared_object(functions: list[tuple[str, bytes]], soname: str | None = None) -> bytes:
    """Build a shared object exporting each ``(name, code)`` as a function.

    Each ``code`` is ARM-state machine code that must be position independent
    with respect to its own function (branches inside one function are
    relative, which the assembler guarantees). Symbol addresses are
    relative to the load base.
    """
    if not functions:
        raise ElfError("a shared object needs at least one exported function")
    names = [name for name, _ in functions]
    if len(set(names)) != len(names):
        raise ElfError("duplicate exported function names")
    for name in names:
        if not name or not name.isascii() or "\0" in name:
            raise ElfError(f"invalid symbol name {name!r}")

    # String table: index 0 is the empty string.
    strtab = bytearray(b"\0")
    name_offsets: dict[str, int] = {}
    for name in names + ([soname] if soname else []):
        if name in name_offsets:
            continue
        name_offsets[name] = len(strtab)
        strtab += name.encode("ascii") + b"\0"
    soname_offset = name_offsets[soname] if soname else None

    nsyms = len(functions) + 1  # plus the null symbol
    nbucket = max(1, len(functions))
    nchain = nsyms

    ehdr_size = _EHDR.size
    phdr_count = 3
    phdr_size = _PHDR.size * phdr_count

    cursor = ehdr_size + phdr_size
    symtab_off = _align(cursor, 4)
    cursor = symtab_off + _SYM.size * nsyms
    strtab_off = cursor
    cursor += len(strtab)
    hash_off = _align(cursor, 4)
    cursor = hash_off + 4 * (2 + nbucket + nchain)

    # Code: each function is 16-byte aligned inside the text segment.
    text_off = _align(cursor, 16)
    code = bytearray()
    symbol_values: list[int] = []
    for _, body in functions:
        code += b"\0" * (_align(text_off + len(code), 16) - (text_off + len(code)))
        symbol_values.append(text_off + len(code))
        code += body
    text_end = text_off + len(code)

    # Dynamic section lives on its own page so its segment is writable.
    dyn_entries = [
        (DT_HASH, hash_off),
        (DT_STRTAB, strtab_off),
        (DT_SYMTAB, symtab_off),
        (DT_STRSZ, len(strtab)),
        (DT_SYMENT, _SYM.size),
    ]
    if soname_offset is not None:
        dyn_entries.append((DT_SONAME, soname_offset))
    dyn_entries.append((DT_NULL, 0))
    dyn_off = _align(text_end, PAGE)
    dyn_size = _DYN.size * len(dyn_entries)
    section_names = [b"", b".dynsym", b".dynstr", b".hash", b".text", b".dynamic", b".shstrtab"]
    shstrtab = bytearray(b"\0")
    shstr_offsets = []
    for section_name in section_names:
        shstr_offsets.append(0 if not section_name else len(shstrtab))
        if section_name:
            shstrtab += section_name + b"\0"
    shstrtab_off = dyn_off + dyn_size
    shoff = _align(shstrtab_off + len(shstrtab), 4)
    file_end = shoff + _SHDR.size * len(section_names)

    # Symbols and hash buckets/chains.
    symtab = bytearray(_SYM.pack(0, 0, 0, 0, 0, 0))
    for name, value, size_source in zip(names, symbol_values, functions):
        symtab += _SYM.pack(name_offsets[name], value, len(size_source[1]),
                            (STB_GLOBAL << 4) | STT_FUNC, 0, _TEXT_SECTION)
    buckets = [0] * nbucket
    chains = [0] * nchain
    for index, name in enumerate(names, start=1):
        bucket = elf_hash(name.encode("ascii")) % nbucket
        chains[index] = buckets[bucket]
        buckets[bucket] = index
    hash_table = struct.pack(f"<{2 + nbucket + nchain}I", nbucket, nchain, *buckets, *chains)

    image = bytearray(file_end)
    phdrs = [
        # Headers, symbols, strings, hash and code: read and execute.
        _PHDR.pack(PT_LOAD, 0, 0, 0, text_end, text_end, PF_R | PF_X, PAGE),
        # Dynamic section: read and write.
        _PHDR.pack(PT_LOAD, dyn_off, dyn_off, dyn_off, dyn_size, dyn_size, PF_R | PF_W, PAGE),
        _PHDR.pack(PT_DYNAMIC, dyn_off, dyn_off, dyn_off, dyn_size, dyn_size, PF_R | PF_W, 4),
    ]
    ehdr = _EHDR.pack(
        b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8,  # ELF32, little-endian, SysV
        ET_DYN,
        EM_ARM,
        1,  # EV_CURRENT
        0,  # entry point: none
        ehdr_size,  # program headers follow the ELF header
        shoff,
        EF_ARM_EABI_VER5,
        ehdr_size,
        _PHDR.size,
        phdr_count,
        _SHDR.size,
        len(section_names),
        len(section_names) - 1,  # .shstrtab
    )
    image[0:ehdr_size] = ehdr
    image[ehdr_size:ehdr_size + phdr_size] = b"".join(phdrs)
    image[symtab_off:symtab_off + len(symtab)] = symtab
    image[strtab_off:strtab_off + len(strtab)] = strtab
    image[hash_off:hash_off + len(hash_table)] = hash_table
    image[text_off:text_end] = code
    dyn = b"".join(_DYN.pack(tag, value) for tag, value in dyn_entries)
    image[dyn_off:dyn_off + len(dyn)] = dyn
    image[shstrtab_off:shstrtab_off + len(shstrtab)] = shstrtab

    # Section table: indices match _TEXT_SECTION and the link fields below.
    sections = [
        (0, SHT_NULL_TYPE, 0, 0, 0, 0, 0, 0, 0, 0),
        (shstr_offsets[1], SHT_DYNSYM, SHF_ALLOC, symtab_off, symtab_off, len(symtab), 2, 1, 4, _SYM.size),
        (shstr_offsets[2], SHT_STRTAB, SHF_ALLOC, strtab_off, strtab_off, len(strtab), 0, 0, 1, 0),
        (shstr_offsets[3], SHT_HASH, SHF_ALLOC, hash_off, hash_off, len(hash_table), 1, 0, 4, 4),
        (shstr_offsets[4], SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, text_off, text_off, len(code), 0, 0, 16, 0),
        (shstr_offsets[5], SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE, dyn_off, dyn_off, dyn_size, 2, 0, 4, _DYN.size),
        (shstr_offsets[6], SHT_STRTAB, 0, shstrtab_off, shstrtab_off, len(shstrtab), 0, 0, 1, 0),
    ]
    for index, section in enumerate(sections):
        image[shoff + index * _SHDR.size:shoff + (index + 1) * _SHDR.size] = _SHDR.pack(*section)
    return bytes(image)
