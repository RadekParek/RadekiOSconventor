"""SjLj LSDA coverage for the playable-game path."""

import unittest
import zipfile
from pathlib import Path

from radek.game import lsda, macho

DATA = Path(__file__).resolve().parent / "data" / "AngryBirds_v1.0_os30.ipa"


class GameLsdaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with zipfile.ZipFile(DATA) as archive:
            cls.image = macho.parse(archive.read("Payload/AngryBirds.app/AngryBirds"))
        cls.parsed, cls.inner, cls.problems = lsda.parse_all(cls.image)

    def test_all_tables_parse(self):
        self.assertEqual(len(self.parsed), 558)
        self.assertEqual(self.problems, [])

    def test_no_absolute_addresses_in_tables(self):
        """Only the 4 typeinfo words are pointers; LPStart is always omitted."""
        type_entries = {}
        for table in self.parsed.values():
            self.assertIsNone(table.lpstart_addr)
            type_entries.update(table.type_entries)
        self.assertEqual(len(type_entries), 4)

    def test_action_chains_are_bounded(self):
        for table in self.parsed.values():
            self.assertGreaterEqual(table.action_base, table.address)
            records = {record for _, _, record in table.actions}
            self.assertEqual(set(table.action_next), records)
            for record, next_record in table.action_next.items():
                if next_record:
                    self.assertIn(next_record, records)

    def test_byte_ranges_cover_tables(self):
        section = self.image.section_named("__DATA", "__gcc_except_tab")
        covered = set()
        for table in self.parsed.values():
            for lo, hi in table.byte_ranges:
                covered.update(range(lo, hi))
            first = min(lo for lo, _ in table.byte_ranges)
            covered.update(range(table.address, first))  # LSDA header bytes
            for addr in table.type_entries:
                covered.update(range(addr, addr + 4))
        for offset in range(section.address, section.address + section.size):
            if offset not in covered:
                self.assertEqual(
                    self.image.read(offset, 1), b"\0",
                    f"unaccounted byte @{offset:#x}")


if __name__ == "__main__":
    unittest.main()
