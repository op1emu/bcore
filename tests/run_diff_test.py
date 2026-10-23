#!/usr/bin/env python3
"""
Differential runner: every linked ELF in build/test_linked/ run twice, by two
emulators or by one with two sets of flags, and the two runs compared: exit
code, stdout and stderr (with --count-packets, the packets and returns too).

Settings that must not change what the guest does are checked this way: a
change against the commit before it (--base-emu), or a feature against the
same binary without it (--base-flags). The test asks for no expected output:
whatever the base does, the other side must do the same, failing tests
included.

  # this build against an older one, in the configurations of --matrix
  run_diff_test.py --base-emu /path/to/old/emu --matrix standard
  # tiered compilation against untiered, one binary
  run_diff_test.py --base-flags "-O 1 --page-table --chain" \\
                   --flags "-O 1 --page-table --chain --tier-threshold 2"
  # the IR each block compiles to (emu --dump), host addresses relative to
  # the base emu reports
  run_diff_test.py --base-emu /path/to/old/emu --dump --flags "-O 2"

A test that times out on both sides counts as the same: what it printed
before the deadline depends on the host's speed.
"""

import argparse
import fnmatch
import hashlib
import os
import re
import shlex
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple


# The configurations of --matrix: (name, flags), the same flags on both sides
# unless the matrix strips some from the base (TIER_ONLY).
MATRICES: Dict[str, List[Tuple[str, str]]] = {
    "standard": [
        ("O0", "-O 0"),
        ("O1-paged-chain", "-O 1 --page-table --chain"),
        ("O2-paged-chain", "-O 2 --page-table --chain"),
        ("O1-paged-steps3", "-O 1 --page-table --max-steps 3"),
    ],
    # Tiered compilation against untiered: the base runs without the tier
    # flags (and without --copy-patch, which only tier 0 uses).
    "tiers": [
        ("O1-t1", "-O 1 --page-table --chain --tier-threshold 1"),
        ("O0-t1-bg0", "-O 0 --page-table --chain --tier-threshold 1 --tier-background 0"),
        ("O1-t2-bg5", "-O 1 --page-table --chain --tier-threshold 2 --tier-background 5"),
        ("O2-t7-bg1", "-O 2 --page-table --chain --tier-threshold 7 --tier-background 1"),
        ("O1-cp-t1", "-O 1 --page-table --chain --tier-threshold 1 --copy-patch"),
        ("O1-cp-t2-ref2", "-O 1 --page-table --chain --tier-threshold 2 --tier-size-ref 2 --copy-patch"),
        ("O1-cp-t1-light", "-O 1 --page-table --chain --tier-threshold 1 --tier1-pipeline light --copy-patch"),
        ("O2-t2-bg1-light", "-O 2 --page-table --chain --tier-threshold 2 --tier-background 1 --tier1-pipeline light"),
    ],
    # The copy-and-patch tier 0 against LLVM's: the base runs without
    # --copy-patch. A threshold of 1e9 keeps every block at tier 0.
    "copy-patch": [
        ("tier0", "-O 1 --page-table --chain --tier-threshold 1000000000 --copy-patch"),
        ("t1", "-O 1 --page-table --chain --tier-threshold 1 --copy-patch"),
        ("t2-bg1", "-O 1 --page-table --chain --tier-threshold 2 --tier-background 1 --copy-patch"),
        ("t2-ref2", "-O 1 --page-table --chain --tier-threshold 2 --tier-size-ref 2 --copy-patch"),
    ],
}
# Flags the base side drops in each matrix (with their values).
BASE_STRIP = {
    "standard": [],
    "tiers": ["--tier-threshold", "--tier-size-ref", "--tier1-pipeline", "--tier-background", "--copy-patch"],
    "copy-patch": ["--copy-patch"],
}
VALUED = {"--tier-threshold", "--tier-size-ref", "--tier1-pipeline", "--tier-background", "-O", "--opt-level", "--max-steps", "--post-pipeline"}

# Lines that describe the run, not the guest: dropped before comparing.
DEFAULT_IGNORE = [r"^copy-patch blocks=\d+ fallbacks=\d+$", r"^cp-runtime "]
# Host addresses in --dump output: emu reports the one the IR adds guest
# addresses to (FlatMemory's buffer less its guest base, which moves from run
# to run), and the decimal constants within a guest address space of it (the
# base plus a guest address, folded with small offsets) are rewritten as
# B+offset. Every other constant, the guest's arithmetic included, is kept.
HOST_BASE = re.compile(rb"^; host fast_base (\d+)$")
NUMBER = re.compile(rb"(?<![-\w.])\d{10,}\b")
HOST_BELOW, HOST_ABOVE = 1 << 32, 1 << 33


def mask_host_addresses(text: bytes, base: Optional[int]) -> bytes:
    if base is None:
        return text

    def mask(match: "re.Match[bytes]") -> bytes:
        offset = int(match.group(0)) - base
        if not -HOST_BELOW <= offset < HOST_ABOVE:
            return match.group(0)
        return b"B+%d" % offset if offset >= 0 else b"B-%d" % -offset
    return NUMBER.sub(mask, text)


class Colors:
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    NC = '\033[0m'


def c(code: str, text: str) -> str:
    return text if not sys.stdout.isatty() else f"{code}{text}{Colors.NC}"


def strip_flags(flags: Sequence[str], names: Sequence[str]) -> List[str]:
    out, skip = [], False
    for flag in flags:
        if skip:
            skip = False
            continue
        if flag in names:
            skip = flag in VALUED
            continue
        out.append(flag)
    return out


@dataclass
class Outcome:
    status: str               # "exit" or "timeout"
    returncode: Optional[int]
    digest: str               # of the compared output
    lines: int
    head: List[str]           # the first lines that differ are looked up in these (--keep)
    host_base: bool = True    # with --dump: whether emu reported its host base


class Runner:
    def __init__(self, timeout: float, ignore: List[re.Pattern], dump: bool, keep_lines: int):
        self.timeout = timeout
        self.ignore = ignore
        self.dump = dump
        self.keep_lines = keep_lines

    def run(self, emu: str, elf: Path, flags: List[str]) -> Outcome:
        """Runs emu on elf, hashing stdout and stderr line by line as they
        come: --dump output reaches gigabytes, all of it compared without
        being kept (only the first keep_lines lines are, for -v)."""
        cmd = [emu, str(elf)] + flags
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        streams, host = {}, {}

        def drain(name, pipe):
            h, n, head = hashlib.sha1(), 0, []
            for line in pipe:
                text = line.rstrip(b"\n")
                if self.dump:
                    found = HOST_BASE.match(text)
                    if found:
                        host["base"] = int(found.group(1))
                        continue
                    text = mask_host_addresses(text, host.get("base"))
                if any(p.search(text.decode(errors="replace")) for p in self.ignore):
                    continue
                h.update(text + b"\n")
                n += 1
                if len(head) < self.keep_lines:
                    head.append(text.decode(errors="replace"))
            streams[name] = (h.hexdigest(), n, head)

        threads = [threading.Thread(target=drain, args=(name, pipe), daemon=True)
                   for name, pipe in (("out", proc.stdout), ("err", proc.stderr))]
        for t in threads:
            t.start()
        try:
            proc.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            for t in threads:
                t.join()
            return Outcome("timeout", None, "", 0, [])
        for t in threads:
            t.join()
        (out, n_out, head_out), (err, n_err, head_err) = streams["out"], streams["err"]
        return Outcome("exit", proc.returncode, out + err, n_out + n_err,
                       ["[stdout] " + l for l in head_out] + ["[stderr] " + l for l in head_err],
                       not self.dump or "base" in host)


def same(a: Outcome, b: Outcome) -> bool:
    if a.status == "timeout" or b.status == "timeout":
        return a.status == b.status
    return a.returncode == b.returncode and a.digest == b.digest


def describe(o: Outcome) -> str:
    if o.status == "timeout":
        return "timeout"
    return f"rc={o.returncode} lines={o.lines}" + ("" if o.host_base else " (no host base: an emu older than --dump's masking)")


def first_difference(a: List[str], b: List[str]) -> Optional[Tuple[int, str, str]]:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i, x, y
    if len(a) != len(b):
        i = min(len(a), len(b))
        return i, a[i] if i < len(a) else "<end>", b[i] if i < len(b) else "<end>"
    return None


def compare(name: str, runner: Runner, base_emu: str, emu: str, base_flags: List[str], flags: List[str],
            files: List[Path], jobs: int, verbose: bool) -> Tuple[int, int, int]:
    """(compared, differing, both timed out) for one configuration."""
    print(f"--- {name}: base {base_emu} {' '.join(base_flags)}")
    print(f"    {'':>{len(name)}}  new  {emu} {' '.join(flags)}")
    t0 = time.monotonic()

    def one(elf: Path):
        return elf, runner.run(base_emu, elf, base_flags), runner.run(emu, elf, flags)

    differing = timeouts = 0
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        for elf, a, b in pool.map(one, files):
            if a.status == "timeout" and b.status == "timeout":
                timeouts += 1
            if same(a, b):
                continue
            differing += 1
            print(c(Colors.RED, "[DIFF]") + f" {elf.name}: base {describe(a)}, new {describe(b)}")
            if verbose:
                found = first_difference(a.head, b.head)
                if found:
                    i, x, y = found
                    print(f"    line {i + 1}:\n      base: {x}\n      new:  {y}")
                elif a.digest != b.digest:
                    print(f"    the first {runner.keep_lines} lines agree; raise --keep-lines to see where")
    took = time.monotonic() - t0
    status = c(Colors.GREEN, "same") if not differing else c(Colors.RED, f"{differing} differ")
    print(f"    {len(files)} tests: {status}" + (f", {timeouts} time out on both sides" if timeouts else "")
          + f" ({took:.0f} s)\n")
    return len(files), differing, timeouts


def main() -> None:
    script_dir = Path(__file__).parent.resolve()
    build_dir = script_dir.parent / "build"
    parser = argparse.ArgumentParser(description="Blackfin emulator differential test: two runs of every ELF compared",
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument("--emu", default=str(build_dir / "emu"), help="the emulator under test (default: build/emu)")
    parser.add_argument("--base-emu", help="the emulator to compare with (default: --emu)")
    parser.add_argument("--flags", help="emu flags of the side under test (default: --base-flags)")
    parser.add_argument("--base-flags", help="emu flags of the base side (default: --flags)")
    parser.add_argument("--matrix", choices=sorted(MATRICES),
                        help="run a set of configurations instead (with --flags appended to both sides)")
    parser.add_argument("--only", metavar="NAMES",
                        help="with --matrix: only these of its configurations (comma-separated)")
    parser.add_argument("--count-packets", action="store_true",
                        help="compare the packets and returns too (emu --count-packets)")
    parser.add_argument("--dump", action="store_true",
                        help="compare the IR of every block (emu --dump), host addresses relative to the base emu reports")
    parser.add_argument("--ignore", action="append", default=[], metavar="REGEX",
                        help="drop output lines matching REGEX before comparing (repeatable; "
                             "copy-patch statistics are always dropped)")
    parser.add_argument("--ignore-list", metavar="FILE",
                        help="ELF basenames to skip (one per line, # comments), as run_emu_test.py")
    parser.add_argument("--filter", help="glob of the test names to run")
    parser.add_argument("-j", "--jobs", type=int, default=0,
                        help="tests run at once, each runs both sides (default: half the CPUs)")
    parser.add_argument("-t", "--timeout", type=float, default=60.0, help="seconds per run (default: 60)")
    parser.add_argument("--keep-lines", type=int, default=2000,
                        help="lines kept per run to show where two differ, with -v (default: 2000)")
    parser.add_argument("-v", "--verbose", action="store_true", help="show the first differing line")
    args = parser.parse_args()

    base_emu = args.base_emu or args.emu
    for emu in {args.emu, base_emu}:
        if not Path(emu).is_file():
            sys.exit(f"error: no emulator at {emu}")
    elf_dir = build_dir / "test_linked"
    if not elf_dir.is_dir():
        sys.exit(f"error: no ELF directory at {elf_dir} (cmake --build build --target link-tests)")
    files = sorted(elf_dir.glob("*.elf"))
    if args.filter:
        files = [f for f in files if fnmatch.fnmatch(f.name, args.filter)]
    if args.ignore_list:
        skip = set()
        for raw in Path(args.ignore_list).read_text().splitlines():
            entry = raw.split("#", 1)[0].strip()
            if entry:
                skip.add(entry)
        files = [f for f in files if f.name not in skip]
    if not files:
        sys.exit("error: no tests to run")

    extra = ["--count-packets"] if args.count_packets else []
    extra += ["--dump"] if args.dump else []
    if args.matrix:
        if args.base_flags:
            sys.exit("error: --matrix decides the base flags: give --flags only (appended to both sides)")
        common = shlex.split(args.flags or "")
        chosen = MATRICES[args.matrix]
        if args.only:
            names = args.only.split(",")
            unknown = [n for n in names if n not in dict(chosen)]
            if unknown:
                sys.exit(f"error: {args.matrix} has no configuration {', '.join(unknown)} "
                         f"(it has {', '.join(n for n, _ in chosen)})")
            chosen = [(n, f) for n, f in chosen if n in names]
        configs = [(name, strip_flags(shlex.split(flags), BASE_STRIP[args.matrix]) + common + extra,
                    shlex.split(flags) + common + extra) for name, flags in chosen]
    else:
        flags = shlex.split(args.flags if args.flags is not None else (args.base_flags or ""))
        base = shlex.split(args.base_flags if args.base_flags is not None else (args.flags or ""))
        if base_emu == args.emu and base == flags:
            sys.exit("error: both sides are the same: give --base-emu, other --base-flags, or --matrix")
        configs = [("compare", base + extra, flags + extra)]

    runner = Runner(args.timeout, [re.compile(p) for p in DEFAULT_IGNORE + args.ignore], args.dump,
                    args.keep_lines)
    jobs = args.jobs or max(1, (os.cpu_count() or 2) // 2)
    print(f"=== Blackfin emulator differential test: {len(files)} tests, {jobs} at once ===\n")
    total_diff = 0
    for name, base_flags, flags in configs:
        _, differing, _ = compare(name, runner, base_emu, args.emu, base_flags, flags, files, jobs, args.verbose)
        total_diff += differing
    if total_diff:
        print(c(Colors.RED, f"FAIL: {total_diff} runs differ"))
        sys.exit(1)
    print(c(Colors.GREEN, f"PASS: {len(configs)} configuration(s), every test the same"))


if __name__ == "__main__":
    main()
