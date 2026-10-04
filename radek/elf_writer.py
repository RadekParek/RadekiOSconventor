"""Minimal self-contained Android ET_DYN writer for translated leaf code.

The output has a single exported function and no imports, relocations, assets, or
Android activity. It is a native-code artifact, not a playable APK. Keeping the
writer deliberately small makes its loader contract auditable and fail-closed.
"""

from __future__ import annotations

import re
import struct

from .archive import InputError


_SYMBOL = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_PAGE_SIZE = 0x1000
_PT_LOAD = 1
_PT_DYNAMIC = 2
_PT_GNU_STACK = 0x6474E551


def _align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def build_shared_object(machine_code: bytes, target_arch: str, symbol: str = "radek_translated_entry") -> bytes:
    """Wrap one relocation-free ARM function in a loadable Android ELF DSO."""
    if target_arch not in ("arm64", "armv7"):
        raise InputError("translated ELF target must be arm64 or armv7")
    if not machine_code or len(machine_code) > 16 * 1024 * 1024:
        raise InputError("translated ELF function is empty or exceeds the 16 MiB limit")
    if not _SYMBOL.fullmatch(symbol):
        raise InputError("invalid translated ELF symbol name")

    is_64 = target_arch == "arm64"
    elf_class = 2 if is_64 else 1
    machine = 183 if is_64 else 40
    word_size = 8 if is_64 else 4
    ehdr_size = 64 if is_64 else 52
    phdr_size = 56 if is_64 else 32
    shdr_size = 64 if is_64 else 40
    dyn_size = 16 if is_64 else 8
    sym_size = 24 if is_64 else 16
    symbol_alignment = 8 if is_64 else 4
    phnum = 4

    phoff = ehdr_size
    text_offset = _align(ehdr_size + phnum * phdr_size, 16)
    text_address = text_offset
    text_end = text_offset + len(machine_code)
    data_offset = _align(text_end, _PAGE_SIZE)
    data_address = data_offset

    dynamic_offset = data_offset
    dynamic_size = 6 * dyn_size
    hash_offset = _align(dynamic_offset + dynamic_size, 4)
    hash_size = 5 * 4
    strings = b"\x00" + symbol.encode("ascii") + b"\x00"
    strings_offset = hash_offset + hash_size
    symbols_offset = _align(strings_offset + len(strings), symbol_alignment)
    symbols_blob_size = 2 * sym_size

    dynamic_address = data_address + dynamic_offset - data_offset
    hash_address = data_address + hash_offset - data_offset
    strings_address = data_address + strings_offset - data_offset
    symbols_address = data_address + symbols_offset - data_offset
    dynamic_entries = (
        (4, hash_address),  # DT_HASH
        (5, strings_address),  # DT_STRTAB
        (6, symbols_address),  # DT_SYMTAB
        (10, len(strings)),  # DT_STRSZ
        (11, sym_size),  # DT_SYMENT
        (0, 0),  # DT_NULL
    )
    if is_64:
        dynamic_blob = b"".join(struct.pack("<qQ", tag, value) for tag, value in dynamic_entries)
    else:
        if any(value > 0xFFFFFFFF for _, value in dynamic_entries):
            raise InputError("translated ARM32 ELF exceeds its address range")
        dynamic_blob = b"".join(struct.pack("<iI", tag, value) for tag, value in dynamic_entries)

    # SysV ELF hash table for exactly one defined global symbol.
    hash_blob = struct.pack("<IIIII", 1, 2, 1, 0, 0)
    if is_64:
        null_symbol = bytes(sym_size)
        function_symbol = struct.pack(
            "<IBBHQQ", 1, 0x12, 0, 1, text_address, len(machine_code)
        )
    else:
        null_symbol = bytes(sym_size)
        function_symbol = struct.pack(
            "<IIIBBH", 1, text_address, len(machine_code), 0x12, 0, 1
        )
    symbols_blob = null_symbol + function_symbol

    shstrtab = b"\x00.text\x00.dynstr\x00.dynsym\x00.hash\x00.dynamic\x00.shstrtab\x00"
    names: dict[str, int] = {}
    cursor = 1
    for section_name in (".text", ".dynstr", ".dynsym", ".hash", ".dynamic", ".shstrtab"):
        names[section_name] = cursor
        cursor += len(section_name) + 1

    strings_end = strings_offset + len(strings)
    symbols_end = symbols_offset + len(symbols_blob)
    data_end = max(dynamic_offset + len(dynamic_blob), hash_offset + len(hash_blob), symbols_end)
    if data_end - data_offset > 16 * 1024 * 1024:
        raise InputError("translated ELF dynamic data exceeds the 16 MiB limit")
    shstrtab_offset = data_end
    section_offset = _align(shstrtab_offset + len(shstrtab), symbol_alignment)
    section_count = 7
    total_size = section_offset + section_count * shdr_size

    image = bytearray(total_size)
    image[text_offset:text_end] = machine_code
    image[dynamic_offset : dynamic_offset + len(dynamic_blob)] = dynamic_blob
    image[hash_offset : hash_offset + len(hash_blob)] = hash_blob
    image[strings_offset:strings_end] = strings
    image[symbols_offset:symbols_end] = symbols_blob
    image[shstrtab_offset : shstrtab_offset + len(shstrtab)] = shstrtab

    ident = b"\x7fELF" + bytes((elf_class, 1, 1, 0, 0)) + bytes(7)
    if len(ident) != 16:
        raise AssertionError("invalid ELF identification size")
    image[:16] = ident
    elf_flags = 0 if is_64 else 0x05000000  # EF_ARM_EABI_VER5
    if is_64:
        struct.pack_into(
            "<HHIQQQIHHHHHH",
            image,
            16,
            3,
            machine,
            1,
            0,
            phoff,
            section_offset,
            elf_flags,
            ehdr_size,
            phdr_size,
            phnum,
            shdr_size,
            section_count,
            6,
        )
        program_headers = (
            (_PT_LOAD, 5, 0, 0, 0, text_end, text_end, _PAGE_SIZE),
            (_PT_LOAD, 6, data_offset, data_address, data_address, data_end - data_offset,
             data_end - data_offset, _PAGE_SIZE),
            (_PT_DYNAMIC, 6, dynamic_offset, dynamic_address, dynamic_address, len(dynamic_blob),
             len(dynamic_blob), word_size),
            (_PT_GNU_STACK, 6, 0, 0, 0, 0, 0, 16),
        )
        for index, header in enumerate(program_headers):
            struct.pack_into("<IIQQQQQQ", image, phoff + index * phdr_size, *header)

        section_headers = [
            (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            (names[".text"], 1, 0x6, text_address, text_offset, len(machine_code), 0, 0, 16, 0),
            (names[".dynstr"], 3, 0x2, strings_address, strings_offset, len(strings), 0, 0, 1, 0),
            (names[".dynsym"], 11, 0x2, symbols_address, symbols_offset, len(symbols_blob), 2, 1,
             symbol_alignment, sym_size),
            (names[".hash"], 5, 0x2, hash_address, hash_offset, len(hash_blob), 3, 0, 4, 4),
            (names[".dynamic"], 6, 0x3, dynamic_address, dynamic_offset, len(dynamic_blob), 2, 0,
             word_size, dyn_size),
            (names[".shstrtab"], 3, 0, 0, shstrtab_offset, len(shstrtab), 0, 0, 1, 0),
        ]
        for index, header in enumerate(section_headers):
            struct.pack_into("<IIQQQQIIQQ", image, section_offset + index * shdr_size, *header)
    else:
        if total_size > 0xFFFFFFFF or data_end > 0xFFFFFFFF:
            raise InputError("translated ARM32 ELF exceeds its file/address range")
        struct.pack_into(
            "<HHIIIIIHHHHHH",
            image,
            16,
            3,
            machine,
            1,
            0,
            phoff,
            section_offset,
            elf_flags,
            ehdr_size,
            phdr_size,
            phnum,
            shdr_size,
            section_count,
            6,
        )
        program_headers = (
            (_PT_LOAD, 0, 0, 0, text_end, text_end, 5, _PAGE_SIZE),
            (_PT_LOAD, data_offset, data_address, data_address, data_end - data_offset,
             data_end - data_offset, 6, _PAGE_SIZE),
            (_PT_DYNAMIC, dynamic_offset, dynamic_address, dynamic_address, len(dynamic_blob),
             len(dynamic_blob), 6, word_size),
            (_PT_GNU_STACK, 0, 0, 0, 0, 0, 6, 16),
        )
        for index, header in enumerate(program_headers):
            struct.pack_into("<IIIIIIII", image, phoff + index * phdr_size, *header)

        section_headers = [
            (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            (names[".text"], 1, 0x6, text_address, text_offset, len(machine_code), 0, 0, 16, 0),
            (names[".dynstr"], 3, 0x2, strings_address, strings_offset, len(strings), 0, 0, 1, 0),
            (names[".dynsym"], 11, 0x2, symbols_address, symbols_offset, len(symbols_blob), 2, 1,
             symbol_alignment, sym_size),
            (names[".hash"], 5, 0x2, hash_address, hash_offset, len(hash_blob), 3, 0, 4, 4),
            (names[".dynamic"], 6, 0x3, dynamic_address, dynamic_offset, len(dynamic_blob), 2, 0,
             word_size, dyn_size),
            (names[".shstrtab"], 3, 0, 0, shstrtab_offset, len(shstrtab), 0, 0, 1, 0),
        ]
        for index, header in enumerate(section_headers):
            struct.pack_into("<IIIIIIIIII", image, section_offset + index * shdr_size, *header)

    return bytes(image)
