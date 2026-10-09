#!/usr/bin/env python3
"""
Packet-count consistency test for CpuState::packets.

Runs every linked ELF in build/test_linked/ through the emulator twice with
--count-packets: once per basic block, and once single-stepped (--max-steps 1,
where every run enters exactly one packet, so the run count is an independent
count of executed packets). The per-block total must equal both single-step
numbers: a block total that only adds static block lengths overcounts every
taken conditional branch and hardware-loop back edge that leaves a block early.

A single-stepped run that times out is reported as skipped (it has no oracle),
never as passed; any other run that prints no packet count fails.
"""

import argparse
import fnmatch
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Optional, Tuple

COUNT_RE = re.compile(r"^packets=(\d+) runs=(\d+)$", re.MULTILINE)


def get_project_paths() -> Tuple[Path, Path]:
    """Return (emu_binary, elf_dir) resolved relative to this script."""
    build_dir = Path(__file__).parent.resolve().parent / "build"
    return build_dir / "emu", build_dir / "test_linked"


class TimedOut(Exception):
    pass


def count(emu: Path, elf: Path, opt_level: int, step: bool,
          timeout: float) -> Optional[Tuple[int, int, int]]:
    """(exit code, packets, runs); None when the run printed no packet count
    (a crash, or a failure before the summary). Raises TimedOut."""
    cmd = [str(emu), "--count-packets", "-O", str(opt_level), str(elf)]
    if step:
        cmd[1:1] = ["--max-steps", "1"]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise TimedOut from error
    m = COUNT_RE.search(proc.stderr)
    if not m:
        return None
    return proc.returncode, int(m.group(1)), int(m.group(2))


def check(emu: Path, elf: Path, opt_level: int, timeout: float) -> Tuple[str, str]:
    """("pass" | "skip" | "fail", reason)."""
    try:
        block = count(emu, elf, opt_level, False, timeout)
    except TimedOut:
        return "fail", "per-block run timed out"
    if block is None:
        return "fail", "per-block run printed no packet count"
    try:
        step = count(emu, elf, opt_level, True, timeout)
    except TimedOut:
        return "skip", "single-step run timed out"
    if step is None:
        return "fail", "single-step run printed no packet count"
    if block[0] != step[0]:
        return "fail", f"exit code differs: block={block[0]} step={step[0]}"
    if step[1] != step[2]:
        return "fail", f"single-step entered {step[1]} packets in {step[2]} runs"
    if block[1] != step[1]:
        return "fail", f"block packets={block[1]} single-step packets={step[1]}"
    return "pass", ""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--filter", help="Glob pattern to filter test names")
    parser.add_argument("-j", "--jobs", type=int, default=0,
                        help="Parallel worker count (default: auto)")
    parser.add_argument("-t", "--timeout", type=float, default=30.0,
                        help="Per-run timeout in seconds (default: 30)")
    parser.add_argument("-O", "--opt-level", type=int, choices=range(4), default=0)
    parser.add_argument("--ignore-list", metavar="FILE",
                        help="File with ELF basenames to skip (# comments allowed)")
    args = parser.parse_args()

    emu, elf_dir = get_project_paths()
    if not emu.is_file() or not elf_dir.is_dir():
        print(f"Error: build {emu} and {elf_dir} first")
        sys.exit(1)

    ignore = set()
    if args.ignore_list:
        for raw in Path(args.ignore_list).read_text().splitlines():
            entry = raw.split("#", 1)[0].strip()
            if entry:
                ignore.add(entry)

    files = sorted(f for f in elf_dir.glob("*.elf") if f.name not in ignore)
    if args.filter:
        files = [f for f in files if fnmatch.fnmatch(f.name, args.filter)]

    workers = args.jobs or os.cpu_count() or 4
    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(
            lambda f: (f.name, check(emu, f, args.opt_level, args.timeout)), files))

    for name, (status, why) in results:
        if status != "pass":
            print(f"[{status.upper()}] {name}: {why}")
    passed = sum(status == "pass" for _, (status, _) in results)
    skipped = sum(status == "skip" for _, (status, _) in results)
    failed = len(results) - passed - skipped
    print(f"-O {args.opt_level}: {passed} consistent, {skipped} skipped, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
