"""Tests for the offline reconstruction layer using synthetic Mach-O images."""

import struct
import tempfile
import unittest
from pathlib import Path

from radek.analysis import analyze
from radek.recon import reconstruct
from radek.recon import objc as objc_mod
from radek.recon import swift as swift_mod
from radek.recon.disasm import decode_arm64
from radek.recon.image import load
from radek.recon.report import blockers, markdown, summary
from radek.recon.source import Reconstructor

from .machobuild import (
    S_ATTR_PURE_INSTRUCTIONS,
    S_ATTR_SOME_INSTRUCTIONS,
    S_LAZY_SYMBOL_POINTERS,
    S_REGULAR,
    S_SYMBOL_STUBS,
    Builder,
    adrp,
    add_imm,
    branch,
    branch_link,
    cbz,
    cmp_reg,
    ldr_imm,
    ldr_literal,
    movz,
    nop,
    ret,
    stp,
    str_imm,
    sub_sp,
)


def write(data: bytes, directory: Path, name: str = "Fixture") -> Path:
    path = directory / name
    path.write_bytes(data)
    return path


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def analyze(self, data: bytes, name: str = "Fixture"):
        path = write(data, self.root, name)
        return path, analyze(path)


def objc_image() -> tuple[Builder, dict]:
    """Build an image with a class, selectors, stubs, Swift metadata and code."""
    builder = Builder()
    text = builder.section(
        "__TEXT", "__text", flags=S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS, at=0x1000
    )
    cstring = builder.section("__TEXT", "__cstring", at=0x1100)
    methname = builder.section("__TEXT", "__objc_methname", at=0x1200)
    classname = builder.section("__TEXT", "__objc_classname", at=0x1300)
    methtype = builder.section("__TEXT", "__objc_methtype", at=0x1320)
    stubs = builder.section(
        "__TEXT",
        "__stubs",
        b"\x1f\x20\x03\xd5" * 9,  # nop; nop; nop
        flags=S_SYMBOL_STUBS | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS,
        reserved2=12,
        at=0x1400,
    )
    swift_types = builder.section("__TEXT", "__swift5_types", at=0x1500)
    swift_fields = builder.section("__TEXT", "__swift5_fieldmd", at=0x1600)
    swift_refl = builder.section("__TEXT", "__swift5_reflstr", at=0x1700)

    const = builder.section("__DATA", "__objc_const", at=0x4000)
    objects = builder.section("__DATA", "__objc_data", at=0x4200)
    classlist = builder.section("__DATA", "__objc_classlist", at=0x4300)
    selrefs = builder.section("__DATA", "__objc_selrefs", at=0x4400)
    classrefs = builder.section("__DATA", "__objc_classrefs", at=0x4500)
    msgrefs = builder.section("__DATA", "__objc_msgrefs", at=0x4600)
    lazy = builder.section("__DATA", "__la_symbol_ptr", flags=S_LAZY_SYMBOL_POINTERS, at=0x4700)
    ivars_section = builder.section("__DATA", "__objc_ivar", at=0x4800)

    cstring.data.extend(b"hello from fixture\x00")
    methname.data.extend(b"doWork\x00other:\x00")
    classname.data.extend(b"MyClass\x00")
    methtype.data.extend(b"v16@0:8\x00")
    swift_refl.data.extend(b"Widget\x00field\x00")

    # Swift field descriptor: named "Widget" with one stored property.
    swift_fields.data.extend(struct.pack("<iiHHI", 0x100001700 - 0x100001600, 0, 0, 12, 1))
    swift_fields.data.extend(struct.pack("<III", 0, 0, 0x100001707 - 0x100001618))

    entry = 0x100001000
    second = entry + 48

    # Objective-C: method list, ivar list, class_ro_t, metaclass and class object.
    method_list = builder.address(const, len(const.data))
    const.data.extend(struct.pack("<II", 24, 2))
    for selector in (0x100001200, 0x100001207):
        const.data.extend(struct.pack("<QQQ", selector, 0x100001320, second))
    ivar_slot = builder.address(ivars_section, len(ivars_section.data))
    ivars_section.data.extend(struct.pack("<I", 8))
    ivar_list = builder.address(const, len(const.data))
    const.data.extend(struct.pack("<II", 32, 1))
    const.data.extend(struct.pack("<QQQII", ivar_slot, 0x100001100, 0x100001320, 3, 8))
    class_ro = builder.address(const, len(const.data))
    const.data.extend(
        struct.pack("<IIIIQQQQQQQ", 0, 8, 16, 0, 0, 0x100001300, method_list, 0, ivar_list, 0, 0)
    )
    meta_ro = builder.address(const, len(const.data))
    const.data.extend(struct.pack("<IIIIQQQQQQQ", 1, 8, 8, 0, 0, 0x100001300, 0, 0, 0, 0, 0))
    meta_class = builder.address(objects, len(objects.data))
    objects.data.extend(struct.pack("<QQQQQ", 0, 0, 0, 0, meta_ro))
    class_object = builder.address(objects, len(objects.data))
    objects.data.extend(struct.pack("<QQQQQ", meta_class, 0, 0, 0, class_ro))
    builder.pointer(classlist, class_object)
    builder.pointer(selrefs, 0x100001200)
    builder.pointer(classrefs, class_object)
    builder.pointer(msgrefs, 0)
    # __objc_msgrefs stores a selector-reference slot here, not the C string.
    builder.pointer(msgrefs, builder.address(selrefs))
    builder.pointer(lazy, 0)
    builder.pointer(lazy, 0)
    builder.pointer(lazy, 0)

    # Swift type descriptor lives in __objc_const; __swift5_types holds the offset to it.
    type_at = builder.address(const, len(const.data))
    const.data.extend(struct.pack("<I", 17))  # flags: struct
    const.data.extend(struct.pack("<i", 0))  # parent
    const.data.extend(struct.pack("<i", 0x100001700 - (type_at + 8)))  # name
    const.data.extend(struct.pack("<i", 0))  # access function
    const.data.extend(struct.pack("<i", 0x100001600 - (type_at + 16)))  # field descriptor
    swift_types.data.extend(struct.pack("<i", type_at - 0x100001500))

    # Code: two functions. The first calls the objc_msgSend and UIKit stubs.
    words = [
        sub_sp(0x20),
        stp(29, 30, 31, 0x10),
        adrp(0, entry + 8, 0x100004500),
        add_imm(0, 0, 0x500),
        ldr_imm(0, 0, 0),
        adrp(1, entry + 20, 0x100004400),
        add_imm(1, 1, 0x400),
        ldr_imm(1, 1, 0),
        branch_link(entry + 32, 0x100001400),
        branch_link(entry + 36, 0x100001418),
        movz(0, 42),
        ret(),
        cmp_reg(0, 1),
        cbz(0, second + 4, second + 16),
        movz(0, 7),
        ret(),
        movz(0, 9),
        ret(),
        0x000000FF,  # unallocated encoding: reported as undecoded
    ]
    text.data.extend(bytes(4 * len(words)))
    for index, word in enumerate(words):
        struct.pack_into("<I", text.data, index * 4, word)
    assert entry + 12 * 4 == second, "second function offset drifted"

    builder.symbols = []
    builder.symbol("_main", entry, section_index=1)
    builder.symbol("-[MyClass doWork]", second, section_index=1)
    builder.symbol("$s7MyClass3fooyySiF", second, section_index=1)
    builder.symbol("_objc_msgSend", 0, section_index=0)
    builder.symbol("_printf", 0, section_index=0)
    builder.symbol("_UIApplicationMain", 0, section_index=0)
    builder.indirect_symbols = [3, 4, 5]
    builder.function_starts = [entry, second]
    builder.entry = entry
    builder.dependencies = ["/System/Library/Frameworks/UIKit.framework/UIKit"]
    return builder, {
        "entry": entry,
        "second": second,
        "class_object": class_object,
        "class_ro": class_ro,
        "stub": 0x100001400,
    }


def swift_mod_dummy() -> int:
    return 0


class ReconstructionTests(Base):
    def test_objective_c_metadata_is_recovered(self):
        builder, _ = objc_image()
        path, info = self.analyze(builder.build())
        image = load(path, info["slices"][0])
        runtime = objc_mod.recover(image)
        self.assertEqual([c.name for c in runtime.classes], ["MyClass"])
        selectors = {m.selector for m in runtime.classes[0].methods}
        self.assertIn("doWork", selectors)
        self.assertIn("other:", selectors)
        self.assertEqual([i.name for i in runtime.classes[0].ivars], ["hello from fixture"])
        self.assertIn("doWork", runtime.selectors)
        self.assertIn("MyClass", runtime.class_references)
        self.assertIn("doWork", runtime.message_selectors)
        self.assertEqual(runtime.message_references[0]["selector"], "doWork")
        self.assertEqual(runtime.message_references[0]["selectorReference"], "0x100004400")
        reconstructor = Reconstructor(image, runtime)
        self.assertEqual(
            reconstructor.describe_address(image.section("__DATA", "__objc_selrefs").address),
            ("objc_selrefs", "doWork", "doWork"),
        )
        self.assertEqual(
            reconstructor.describe_address(image.section("__DATA", "__objc_msgrefs").address),
            ("objc_msgrefs", "doWork", "doWork"),
        )

    def test_relative_small_method_lists(self):
        builder = Builder()
        methname = builder.section("__TEXT", "__objc_methname", at=0x1100)
        const = builder.section("__DATA", "__objc_const", at=0x4000)
        methname.data.extend(b"smallMethod\x00")
        list_address = builder.address(const, len(const.data))
        const.data.extend(struct.pack("<II", 0x80000000 | 12, 1))
        entry = len(const.data)
        const.data.extend(struct.pack("<iii", 0, 0, 0))
        target = builder.address(methname, 0)
        struct.pack_into("<i", const.data, entry, target - (list_address + 8))
        path, info = self.analyze(builder.build())
        image = load(path, info["slices"][0])
        methods = objc_mod._methods(image, list_address, "Small", "instance")
        self.assertEqual([m.selector for m in methods], ["smallMethod"])

    def test_function_discovery_and_control_flow(self):
        builder, meta = objc_image()
        path, info = self.analyze(builder.build())
        result = reconstruct(path.parent, {path.name: info})
        self.assertEqual(result["status"], "ok")
        slice_data = result["images"][0]["slices"][0]
        stats = slice_data["disassembly"]
        self.assertEqual(stats["functions"], 2)
        self.assertGreaterEqual(stats["instructions"], 10)
        # The shifted-register add/sub and logical decoders now cover SUBS/ORR,
        # so the fixture's cmp decodes; only genuinely unallocated encodings are
        # reported as undecoded.
        self.assertEqual(stats["unknownInstructions"], 0)
        self.assertTrue(decode_arm64(0x000000FF, 0).unknown)
        self.assertIn("cmp", "\n".join(
            line for f in slice_data["functions"] for line in f["listing"]
        ))
        self.assertEqual(slice_data["entryPoint"], f"0x{meta['entry']:x}")
        names = {f["name"] for f in slice_data["functions"]}
        self.assertEqual(names, {"_main", "-[MyClass doWork]"})
        main = next(f for f in slice_data["functions"] if f["name"] == "_main")
        listing = "\n".join(main["listing"])
        self.assertIn("objc_msgSend", listing)
        self.assertIn("@selector(doWork)", listing)
        other = next(f for f in slice_data["functions"] if f["name"] == "-[MyClass doWork]")
        # SUBS is decoded now, so the pseudocode shows the comparison instead of
        # an undecoded encoding; unallocated words are still marked unknown above.
        self.assertIn("cmp", "\n".join(other["listing"]))

    def test_used_apis_are_attributed_and_unused_are_separated(self):
        builder, _ = objc_image()
        path, info = self.analyze(builder.build())
        result = reconstruct(path.parent, {path.name: info})
        apis = result["images"][0]["slices"][0]["apis"]
        used = {item["name"]: item for item in apis["used"]}
        self.assertIn("_objc_msgSend", used)
        self.assertEqual(used["_objc_msgSend"]["feasibility"], "compatibility")
        self.assertIn("_printf", apis["unused"])
        self.assertIn("UIKit", apis["linkedFrameworks"])
        self.assertGreaterEqual(apis["linkedFrameworks"]["UIKit"]["symbolsUsed"], 1)
        self.assertIn("byFeasibility", apis)

    def test_swift_metadata_and_demangling(self):
        builder, meta = objc_image()
        path, info = self.analyze(builder.build())
        result = reconstruct(path.parent, {path.name: info})
        swift_data = result["images"][0]["slices"][0]["swift"]
        self.assertIn("__swift5_types", swift_data["sections"])
        self.assertGreaterEqual(swift_data["typeCount"], 1)
        self.assertEqual(swift_data["types"][0]["kind"], "struct")
        self.assertIn("field", swift_data["types"][0]["fields"])
        self.assertGreaterEqual(swift_data["symbolCount"], 1)
        self.assertTrue(swift_mod.demangle("$s7MyClass3fooyySiF").startswith("MyClass.foo"))
        self.assertIn("partial", swift_mod.demangle("$s7MyClass3foo"))
        self.assertEqual(swift_mod.demangle("_notMangled"), "_notMangled")

    def test_report_rendering(self):
        builder, _ = objc_image()
        path, info = self.analyze(builder.build())
        result = reconstruct(path.parent, {path.name: info})
        text = markdown(result, {"name": "Fixture", "bundleId": "org.example"})
        self.assertIn("IPA reconstruction report", text)
        self.assertIn("Architecture arm64", text)
        self.assertIn("MyClass", text)
        self.assertIn("Reconstructed coverage", text)
        stats = summary(result)
        self.assertEqual(stats["architectures"], ["arm64"])
        self.assertEqual(stats["functionCount"], 2)
        self.assertGreater(stats["coverage"], 0)
        messages = blockers(result)
        self.assertTrue(any("compatibility layer" in m for m in messages))

    def test_reconstruction_never_crashes_on_garbage(self):
        builder = Builder()
        text = builder.section(
            "__TEXT",
            "__text",
            b"\x00" * 64,
            flags=S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS,
            at=0x1000,
        )
        builder.function_starts = [builder.address(text, 0)]
        path, info = self.analyze(builder.build())
        result = reconstruct(path.parent, {path.name: info})
        self.assertEqual(result["status"], "ok")


class DisassemblyTests(Base):
    def test_branch_targets_and_returns(self):
        from radek.recon.disasm import decode_arm64, decode_thumb

        self.assertEqual(decode_arm64(ret(), 0x1000).kind, "ret")
        self.assertEqual(decode_arm64(movz(0, 42), 0x1000).immediate, 42)
        self.assertEqual(decode_arm64(branch(0x1000, 0x2000), 0x1000).target, 0x2000)
        call = decode_arm64(branch_link(0x1000, 0x2000), 0x1000)
        self.assertEqual((call.kind, call.target), ("call", 0x2000))
        conditional = decode_arm64(cbz(0, 0x1000, 0x1100), 0x1000)
        self.assertEqual((conditional.kind, conditional.target, conditional.condition), ("branch", 0x1100, None))
        page = decode_arm64(adrp(0, 0x100001000, 0x100004000), 0x100001000)
        self.assertEqual((page.kind, page.immediate), ("adrp", 0x100004000))
        self.assertRaises(ValueError, adrp, 0, 0x1000, 0x900000000)
        literal = decode_arm64(ldr_literal(1, 0x1000, 0x1010), 0x1000)
        self.assertEqual((literal.kind, literal.target), ("loadlit", 0x1010))
        store = decode_arm64(str_imm(0, 31, 16), 0x1000)
        self.assertEqual(store.kind, "store")
        load = decode_arm64(ldr_imm(0, 31, 16), 0x1000)
        self.assertEqual((load.kind, load.immediate), ("load", 16))
        self.assertTrue(decode_arm64(nop(), 0x1000).kind == "system")
        unknown = decode_arm64(0x000000FF, 0x1000)
        self.assertTrue(unknown.unknown)
        thumb = decode_thumb(struct.pack("<HH", 0x202A, 0x4770), 0x1000, 0)
        self.assertEqual(thumb.mnemonic, "mov")
        self.assertEqual(decode_thumb(struct.pack("<H", 0x4770), 0x1000, 0).kind, "ret")
