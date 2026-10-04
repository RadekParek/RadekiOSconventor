"""Small auditable Mach-O/IPA fixtures. These are not signed installable iOS apps."""

import plistlib
import struct
import zipfile
from pathlib import Path
from radek.resources import fallback_icon


def macho(
    code=None,
    cpu=0x100000C,
    subtype=0,
    dependencies=(),
    encrypted=False,
    thumb=False,
    imports=(),
    reloc=False,
    extras=(),
    blobs=None,
    section_name="__text",
):
    wide = cpu == 0x100000C
    if code is None:
        code = (
            struct.pack("<II", 0x52800540, 0xD65F03C0) if wide else struct.pack("<II", 0xE3A0002A, 0xE12FFF1E)
        )
    strings = b"\x00_main\x00"
    symbols = struct.pack(
        "<IBBHQ" if wide else "<IBBHI",
        1,
        0x0F,
        1,
        8 if thumb else 0,
        (0x100000000 if wide else 0x10000) + 0x1000,
    )
    for name in imports:
        index = len(strings)
        strings += name.encode() + b"\x00"
        symbols += struct.pack("<IBBHQ" if wide else "<IBBHI", index, 1, 0, 0x100, 0)
    symoff = 0x1000 + ((len(code) + 7) // 8 * 8)
    stroff = symoff + len(symbols)
    data = bytearray(stroff + len(strings))
    data[0x1000 : 0x1000 + len(code)] = code
    data[symoff:stroff] = symbols
    data[stroff:] = strings
    if reloc:
        if len(data) < 0x3008:
            data.extend(b"\x00" * (0x3008 - len(data)))
        struct.pack_into("<II", data, 0x3000, 0, 0x1D000000)
    for off, blob in (blobs or {}).items():
        if len(data) < off + len(blob):
            data.extend(b"\x00" * (off + len(blob) - len(data)))
        data[off : off + len(blob)] = blob
    name = section_name.encode().ljust(16, b"\x00")
    segment_name = b"__TEXT".ljust(16, b"\x00")
    if wide:
        section = struct.pack(
            "<16s16sQQIIIIIIII",
            name,
            segment_name,
            0x100001000,
            len(code),
            0x1000,
            2,
            0x3000 if reloc else 0,
            int(reloc),
            0x80000400,
            0,
            0,
            0,
        )
        segment = (
            struct.pack(
                "<II16sQQQQIIII", 0x19, 152, segment_name, 0x100000000, 0x10000, 0, len(data), 5, 5, 1, 0
            )
            + section
        )
    else:
        section = struct.pack(
            "<16s16sIIIIIIIII",
            name,
            segment_name,
            0x11000,
            len(code),
            0x1000,
            1 if thumb else 2,
            0x3000 if reloc else 0,
            int(reloc),
            0x80000400,
            0,
            0,
        )
        segment = (
            struct.pack("<II16sIIIIIIII", 1, 124, segment_name, 0x10000, 0x10000, 0, len(data), 5, 5, 1, 0)
            + section
        )
    commands = [
        segment,
        struct.pack("<IIQQ", 0x80000028, 24, 0x1000, 0),
        struct.pack("<IIIIII", 2, 24, symoff, 1 + len(imports), stroff, len(strings)),
    ]
    for dep in dependencies:
        name = dep.encode() + b"\x00"
        size = (24 + len(name) + 7) // 8 * 8
        commands.append(
            struct.pack("<IIIIII", 0xC, size, 24, 0, 0x10000, 0x10000) + name.ljust(size - 24, b"\x00")
        )
    if encrypted:
        commands.append(struct.pack("<IIIIII", 0x2C if wide else 0x21, 24, 0x1000, len(code), 1, 0))
    commands.extend(extras)
    header = struct.pack(
        "<IIIIIII",
        0xFEEDFACF if wide else 0xFEEDFACE,
        cpu,
        subtype,
        2,
        len(commands),
        sum(map(len, commands)),
        0,
    )
    if wide:
        header += bytes(4)
    combined = header + b"".join(commands)
    assert len(combined) < 0x1000
    data[: len(combined)] = combined
    return bytes(data)


def fat(slices, wide=False, little=False):
    endian = "<" if little else ">"
    out = bytearray(4096)
    struct.pack_into(endian + "II", out, 0, 0xCAFEBABF if wide else 0xCAFEBABE, len(slices))
    for i, data in enumerate(slices):
        offset = len(out)
        cpu, subtype = struct.unpack_from("<II", data, 4)
        if wide:
            struct.pack_into(endian + "IIQQII", out, 8 + i * 32, cpu, subtype, offset, len(data), 12, 0)
        else:
            struct.pack_into(endian + "IIIII", out, 8 + i * 20, cpu, subtype, offset, len(data), 12)
        out.extend(data)
        out.extend(bytes((-len(out)) % 4096))
    return bytes(out)


# Fixed timestamp so committed fixture IPAs are byte-for-byte reproducible.
_FIXED_ZIP_TIME = (2020, 1, 1, 0, 0, 0)


def _write(z: zipfile.ZipFile, name: str, data: bytes | str) -> None:
    info = zipfile.ZipInfo(name, date_time=_FIXED_ZIP_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    z.writestr(info, data)


def ipa(path: Path, executable=None, binary=True, extra=None, icon=True):
    info = {
        "CFBundleExecutable": "Fixture",
        "CFBundleIdentifier": "org.example.synthetic",
        "CFBundleName": "Native Fixture",
        "CFBundleDisplayName": "Native Fixture",
        "CFBundleVersion": "1",
        "CFBundleShortVersionString": "1.0",
        "MinimumOSVersion": "8.0",
        "CFBundleIcons": {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["AppIcon"]}},
    }
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        _write(
            z,
            "Payload/Fixture.app/Info.plist",
            plistlib.dumps(info, fmt=plistlib.FMT_BINARY if binary else plistlib.FMT_XML),
        )
        _write(z, "Payload/Fixture.app/Fixture", executable or macho())
        if icon:
            _write(z, "Payload/Fixture.app/AppIcon@2x.png", fallback_icon())
        _write(z, "Payload/Fixture.app/en.lproj/Localizable.strings", b'"hello" = "Hello";')
        _write(z, "Payload/Fixture.app/config.json", b'{"fixture":true}')
        for name, content in (extra or {}).items():
            _write(z, name, content)
    return path
