#!/usr/bin/env python3
"""Tests of capped_run.run_capped, with sh and coreutils for the runs:
python3 -m unittest tests/test_capped_run.py"""

import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from capped_run import run_capped  # noqa: E402


def gone(pid: int, within: float = 2.0) -> bool:
    """Whether pid has stopped running (a zombie no longer runs) in time."""
    end = time.monotonic() + within
    while time.monotonic() < end:
        try:
            with open(f"/proc/{pid}/stat") as stat:
                if stat.read().rsplit(")", 1)[1].split()[0] == "Z":
                    return True
        except (FileNotFoundError, ProcessLookupError):  # gone before the open, or before the read
            return True
        time.sleep(0.05)
    return False


class RunCappedTest(unittest.TestCase):
    def setUp(self):
        self.threads = threading.active_count()

    def tearDown(self):
        self.assertEqual(threading.active_count(), self.threads, "no reader thread outlives its run")

    def test_exit_code_and_output(self):
        r = run_capped(["sh", "-c", "echo out; echo err >&2; exit 3"], timeout=10)
        self.assertEqual((r.returncode, r.stdout, r.stderr), (3, "out\n", "err\n"))
        self.assertFalse(r.timed_out or r.flooded)

    def test_timeout(self):
        start = time.monotonic()
        r = run_capped(["sleep", "30"], timeout=0.2)
        self.assertTrue(r.timed_out)
        self.assertIsNone(r.returncode)
        self.assertLess(time.monotonic() - start, 2)

    def test_flood_is_stopped_at_the_limit(self):
        start = time.monotonic()
        r = run_capped(["yes"], timeout=30, limit=1 << 20)
        self.assertTrue(r.flooded)
        self.assertIsNone(r.returncode)
        self.assertEqual(len(r.stdout), 1 << 20)
        self.assertLess(time.monotonic() - start, 10)

    def test_output_of_a_descendant_that_finishes_in_time(self):
        # The run exits at once; what it leaves running writes later, and is
        # read: the pipes close when every process holding them is gone.
        r = run_capped(["sh", "-c", "(sleep 0.3; echo late) & echo early"], timeout=10)
        self.assertEqual((r.returncode, r.stdout), (0, "early\nlate\n"))
        self.assertFalse(r.timed_out)

    def test_descendant_holding_the_pipes_times_out(self):
        # The run exits 0 at once, a descendant keeps its stdout: the run is
        # not over, and at the timeout it is stopped, descendant and all.
        start = time.monotonic()
        r = run_capped(["sh", "-c", "sleep 30 & echo $!; exit 0"], timeout=0.3)
        self.assertLess(time.monotonic() - start, 3)
        self.assertTrue(r.timed_out)
        self.assertIsNone(r.returncode)
        self.assertTrue(gone(int(r.stdout.split()[0])), "the descendant is killed")

    def test_flooding_descendant_is_killed(self):
        r = run_capped(["sh", "-c", "yes & echo $! >&2; wait"], timeout=30, limit=1 << 20)
        self.assertTrue(r.flooded)
        self.assertTrue(gone(int(r.stderr.split()[0])), "the descendant is killed")


if __name__ == "__main__":
    unittest.main()
