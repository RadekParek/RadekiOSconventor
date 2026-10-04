import os
import plistlib
import stat
import tempfile
import unittest
import zipfile
from pathlib import Path
from radek.archive import *
from .fixtures import ipa


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_xml_and_binary_metadata_and_icon(self):
        for binary in (True, False):
            source = ipa(self.root / f"{binary}.ipa", binary=binary)
            dest = self.root / str(binary)
            extract_ipa(source, dest)
            app = discover_app(dest)
            info = read_plist(app / "Info.plist")
            self.assertEqual(info["CFBundleExecutable"], "Fixture")
            self.assertEqual(metadata(info, source)["version"], "1.0")
            self.assertEqual(metadata(info, source)["minimumIOSVersion"], "8.0")
            self.assertEqual(icon_candidates(info, app)[0].name, "AppIcon@2x.png")

    def bad_zip(self, name, data=b"x", attr=None):
        source = self.root / "bad.ipa"
        dest = self.root / "bad"
        with zipfile.ZipFile(source, "w") as z:
            info = zipfile.ZipInfo(name)
            if attr is not None:
                info.external_attr = attr
            z.writestr(info, data)
        with self.assertRaises((InputError, ValueError)):
            extract_ipa(source, dest)
        self.assertFalse(dest.exists())

    def test_traversal(self):
        for name in ("../outside", "/absolute", "Payload/../a", "C:/windows", "a\\b", "a//b", "./a"):
            with self.subTest(name=name):
                self.bad_zip(name)

    def test_symlink(self):
        self.bad_zip("link", b"../../outside", (stat.S_IFLNK | 0o777) << 16)

    def test_special_file(self):
        self.bad_zip("fifo", b"", (stat.S_IFIFO | 0o600) << 16)

    def test_case_collision(self):
        source = self.root / "case.ipa"
        with zipfile.ZipFile(source, "w") as z:
            z.writestr("Icon.png", "a")
            z.writestr("icon.png", "b")
        with self.assertRaises(InputError):
            extract_ipa(source, self.root / "case")
        self.assertFalse((self.root / "case").exists())

    def test_no_fixed_archive_size_limit(self):
        """An IPA is bounded by free storage, not by a number picked in advance."""
        source = ipa(self.root / "test.ipa")
        size = source.stat().st_size
        self.assertGreater(size, 0)
        # The old hard cap was 512 MiB; a larger archive must not be rejected
        # just for being large. Only the free-space guard may refuse it.
        self.assertFalse(hasattr(Limits(), "archive_bytes"))
        extract_ipa(source, self.root / "dest")
        self.assertTrue((self.root / "dest").is_dir())

    def test_requires_free_space_for_large_archives(self):
        source = ipa(self.root / "test.ipa")
        with self.assertRaises(InputError) as caught:
            require_free_space(source, 1 << 62)
        self.assertIn("not enough free storage", str(caught.exception))

    def test_size_limit(self):
        source = ipa(self.root / "test.ipa")
        with self.assertRaises(InputError):
            extract_ipa(source, self.root / "dest", Limits(file_bytes=100))

    def test_bomb(self):
        source = self.root / "bomb.ipa"
        with zipfile.ZipFile(source, "w", compression=zipfile.ZIP_DEFLATED) as z:
            z.writestr("big", bytes(1000000))
        with self.assertRaises(InputError):
            extract_ipa(source, self.root / "bomb")

    def test_malformed_archive(self):
        source = self.root / "bad.ipa"
        source.write_bytes(b"PKbad")
        with self.assertRaises(zipfile.BadZipFile):
            extract_ipa(source, self.root / "bad")
        self.assertFalse((self.root / "bad").exists())

    def test_ambiguous_app(self):
        root = self.root / "ex"
        (root / "Payload/one.app").mkdir(parents=True)
        (root / "Payload/two.app").mkdir()
        with self.assertRaises(InputError):
            discover_app(root)

    def test_plist_executable_escape(self):
        p = self.root / "Info.plist"
        p.write_bytes(plistlib.dumps({"CFBundleIdentifier": "org.test", "CFBundleExecutable": "../bad"}))
        with self.assertRaises(InputError):
            read_plist(p)

    def test_plist_malformed(self):
        p = self.root / "Info.plist"
        p.write_bytes(b"bplist00bad")
        with self.assertRaises(InputError):
            read_plist(p)

    def test_existing_workspace_not_overwritten(self):
        source = ipa(self.root / "test.ipa")
        target = self.root / "existing"
        target.mkdir()
        (target / "sentinel").write_text("safe")
        with self.assertRaises(FileExistsError):
            extract_ipa(source, target)
        self.assertEqual((target / "sentinel").read_text(), "safe")

    def test_nonstring_metadata_rejected(self):
        p = self.root / "Info.plist"
        p.write_bytes(
            plistlib.dumps(
                {"CFBundleIdentifier": "org.test", "CFBundleExecutable": "Main", "CFBundleVersion": b"bad"}
            )
        )
        with self.assertRaises(InputError):
            read_plist(p)

    def test_xml_entity_rejected(self):
        p = self.root / "Info.plist"
        p.write_bytes(
            b'<!DOCTYPE plist [<!ENTITY a SYSTEM "file:///etc/passwd">]><plist><dict><key>CFBundleExecutable</key><string>&a;</string></dict></plist>'
        )
        with self.assertRaises(InputError):
            read_plist(p)
