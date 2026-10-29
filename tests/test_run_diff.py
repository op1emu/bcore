#!/usr/bin/env python3
"""Tests of run_diff_test.py's comparison, with fake emulators (shell
scripts that print a fixed output): python3 -m unittest tests/test_run_diff.py"""

import stat
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import run_diff_test as rd  # noqa: E402

SATURATION = "%acc_sat = select i1 %acc_av, i64 {}, i64 %752"


BASE = 0x7d2b527ff010 - 0x07f00000


class MaskTest(unittest.TestCase):
    def test_host_addresses_relative_to_the_base(self):
        for line, masked in [
            (f"add i64 {BASE}, %26", "add i64 B+0, %26"),
            (f"inttoptr (i64 {BASE + 0xFFA00000} to ptr)", f"inttoptr (i64 B+{0xFFA00000} to ptr)"),
            (f"getelementptr i8, ptr %p, i64 {BASE - 4}", "getelementptr i8, ptr %p, i64 B-4"),
        ]:
            self.assertEqual(rd.mask_host_addresses(line.encode(), BASE).decode(), masked)

    def test_guest_constants_kept(self):
        # Constants the ISA tests' IR holds: 40-bit saturation bounds, and
        # the 64-bit patterns of byte unpacking and sign masks at -O 2, some
        # in the range of host addresses.
        for line in [
            SATURATION.format(549755813887),
            "and i64 %x, -549755813888",
            "lshr <2 x i64> <i64 2848996380690057489, i64 43472234812775>, %21",
            "and i64 %140, 9223090561878065152",
            f"and i64 %x, {0x6759ab2d4537}",
            f"and i64 %x, -{BASE}",
            f"%v{BASE} = add i64 %a, 1",
        ]:
            self.assertEqual(rd.mask_host_addresses(line.encode(), BASE).decode(), line)

    def test_window(self):
        self.assertEqual(rd.mask_host_addresses(f"i64 {BASE - (1 << 32)}".encode(), BASE), b"i64 B-4294967296")
        self.assertEqual(rd.mask_host_addresses(f"i64 {BASE - (1 << 32) - 1}".encode(), BASE),
                         f"i64 {BASE - (1 << 32) - 1}".encode())
        self.assertEqual(rd.mask_host_addresses(f"i64 {BASE + (1 << 33)}".encode(), BASE),
                         f"i64 {BASE + (1 << 33)}".encode())

    def test_no_base_no_masking(self):
        self.assertEqual(rd.mask_host_addresses(f"add i64 {BASE}, %26".encode(), None), f"add i64 {BASE}, %26".encode())


class CompareTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.elf = Path(self.dir.name) / "t.elf"
        self.elf.write_bytes(b"")

    def tearDown(self):
        self.dir.cleanup()

    def emu(self, name: str, script: str) -> str:
        path = Path(self.dir.name) / name
        path.write_text("#!/bin/sh\n" + script)
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return str(path)

    def outcome(self, emu: str, dump: bool = True, ignore=()):
        runner = rd.Runner(30, [rd.re.compile(p) for p in ignore], dump, keep_lines=10)
        return runner.run(emu, self.elf, [])

    def ir(self, name: str, base: int, *lines: str) -> str:
        """A fake emu --dump: its host base, then lines of IR."""
        body = f"echo '; host fast_base {base}' >&2\n" if base is not None else ""
        body += "".join(f"echo '{line}' >&2\n" for line in lines)
        return self.emu(name, body + "echo OK\n")

    def test_saturation_constant_change_differs_with_dump(self):
        a = self.ir("a", BASE, SATURATION.format(549755813887))
        b = self.ir("b", BASE + 0x100000, SATURATION.format(549755813886))
        self.assertFalse(rd.same(self.outcome(a), self.outcome(b)))

    def test_host_base_moves_same_with_dump(self):
        a = self.ir("a", BASE, f"add i64 {BASE + 0x1000}, %26")
        b = self.ir("b", BASE + 0x2345000, f"add i64 {BASE + 0x2345000 + 0x1000}, %26")
        self.assertTrue(rd.same(self.outcome(a), self.outcome(b)))
        self.assertFalse(rd.same(self.outcome(a, dump=False), self.outcome(b, dump=False)))

    def test_guest_address_change_differs_with_dump(self):
        a = self.ir("a", BASE, f"add i64 {BASE + 0x1000}, %26")
        b = self.ir("b", BASE + 0x2345000, f"add i64 {BASE + 0x2345000 + 0x1004}, %26")
        self.assertFalse(rd.same(self.outcome(a), self.outcome(b)))

    def test_emu_without_a_base_differs_and_says_so(self):
        a = self.ir("a", None, f"add i64 {BASE}, %26")
        b = self.ir("b", BASE + 0x1000, f"add i64 {BASE + 0x1000}, %26")
        oa, ob = self.outcome(a), self.outcome(b)
        self.assertFalse(rd.same(oa, ob))
        self.assertIn("no host base", rd.describe(oa))
        self.assertNotIn("no host base", rd.describe(ob))

    def test_output_past_a_mebibyte_compared(self):
        a = self.emu("a", "echo OK\n")
        b = self.emu("b", "echo OK\nhead -c 3000000 /dev/zero | tr '\\0' x\necho\n")
        self.assertFalse(rd.same(self.outcome(a), self.outcome(b)))

    def test_late_difference_in_long_output(self):
        body = "i=0\nwhile [ $i -lt 20000 ]; do echo 'line of output padded to some length'; i=$((i+1)); done\n"
        a = self.emu("a", body + "echo end-a\n")
        b = self.emu("b", body + "echo end-b\n")
        self.assertFalse(rd.same(self.outcome(a), self.outcome(b)))
        self.assertTrue(rd.same(self.outcome(a), self.outcome(a)))

    def test_ignored_lines_dropped(self):
        a = self.emu("a", "echo 'copy-patch blocks=3 fallbacks=1'\necho 'cp-runtime built=2 refused=0'\necho OK\n")
        b = self.emu("b", "echo OK\n")
        ignore = rd.DEFAULT_IGNORE
        self.assertTrue(rd.same(self.outcome(a, ignore=ignore), self.outcome(b, ignore=ignore)))

    def test_exit_code_compared(self):
        a = self.emu("a", "echo OK\nexit 0\n")
        b = self.emu("b", "echo OK\nexit 1\n")
        self.assertFalse(rd.same(self.outcome(a), self.outcome(b)))


class ShardTest(unittest.TestCase):
    FILES = [Path(f"t{i}.elf") for i in range(7)]

    def test_every_nth(self):
        self.assertEqual(rd.shard(self.FILES, "2/3"), [Path("t1.elf"), Path("t4.elf")])

    def test_shards_cover_every_test_once(self):
        shards = [rd.shard(self.FILES, f"{k}/3") for k in (1, 2, 3)]
        self.assertEqual(sorted(sum(shards, [])), sorted(self.FILES))

    def test_bad_spec(self):
        for spec in ["0/2", "3/2", "2", "a/b"]:
            with self.assertRaises(SystemExit):
                rd.shard(self.FILES, spec)


if __name__ == "__main__":
    unittest.main()
