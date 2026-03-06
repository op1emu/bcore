#!/usr/bin/env python3
"""
Emulator test runner for bfin JIT emulator.
Runs each linked ELF in build/test_linked/ through the emulator and classifies
the result as pass (exit 0), fail (exit 1), timeout, or error (crash/signal).
"""

import argparse
import fnmatch
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional


# ANSI colors (matches run_comparison_test.py style)
class Colors:
    RED     = '\033[0;31m'
    GREEN   = '\033[0;32m'
    YELLOW  = '\033[1;33m'
    MAGENTA = '\033[0;35m'
    NC      = '\033[0m'


def no_color() -> bool:
    return not sys.stdout.isatty()


def c(code: str, text: str) -> str:
    if no_color():
        return text
    return f"{code}{text}{Colors.NC}"


@dataclass
class EmuResult:
    name: str
    status: str          # "pass", "fail", "timeout", "error"
    returncode: int
    stdout: str
    stderr: str
    duration: float      # seconds


@dataclass
class Summary:
    total: int = 0
    passed: int = 0
    failed: int = 0
    timed_out: int = 0
    errors: int = 0
    ignored: int = 0
    ignored_but_passing: List[str] = field(default_factory=list)
    failures: List[str] = field(default_factory=list)


def get_project_paths() -> tuple[Path, Path]:
    """Return (emu_binary, elf_dir) resolved relative to this script."""
    script_dir = Path(__file__).parent.resolve()
    project_root = script_dir.parent
    build_dir = project_root / "build"
    return build_dir / "emu", build_dir / "test_linked"


def run_one(elf_path: Path, timeout: float) -> EmuResult:
    name = elf_path.name
    t0 = time.monotonic()
    try:
        proc = subprocess.run(
            [str(elf_path.parent.parent.parent / "build" / "emu"), str(elf_path)],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        duration = time.monotonic() - t0
        rc = proc.returncode
        if rc == 0:
            status = "pass"
        elif rc == 1:
            status = "fail"
        else:
            status = "error"
        return EmuResult(name, status, rc, proc.stdout, proc.stderr, duration)
    except subprocess.TimeoutExpired as exc:
        duration = time.monotonic() - t0
        stdout = (exc.stdout or b"").decode(errors="replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        stderr = (exc.stderr or b"").decode(errors="replace") if isinstance(exc.stderr, bytes) else (exc.stderr or "")
        return EmuResult(name, "timeout", -1, stdout, stderr, duration)


def print_result(r: EmuResult, verbose: bool) -> None:
    tag_map = {
        "pass":    c(Colors.GREEN,   "[PASS]"),
        "fail":    c(Colors.RED,     "[FAIL]"),
        "timeout": c(Colors.YELLOW,  "[TIME]"),
        "error":   c(Colors.MAGENTA, "[ERR ]"),
    }
    tag = tag_map.get(r.status, "[????]")
    print(f"{tag} {r.name}  ({r.duration:.2f}s)")
    if verbose and r.status != "pass":
        if r.stdout.strip():
            print("  stdout:")
            for line in r.stdout.rstrip().splitlines():
                print(f"    {line}")
        if r.stderr.strip():
            print("  stderr:")
            for line in r.stderr.rstrip().splitlines():
                print(f"    {line}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Blackfin JIT emulator test runner",
    )
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Print stdout/stderr for non-passing tests")
    parser.add_argument("-f", "--file",
                        help="Run only a specific ELF file (path or basename)")
    parser.add_argument("--filter",
                        help="Glob pattern to filter test names (e.g. 'cc-*')")
    parser.add_argument("-F", "--fail-fast", action="store_true", default=True,
                        dest="fail_fast",
                        help="Stop at first failure (default: enabled)")
    parser.add_argument("--no-fail-fast", action="store_false", dest="fail_fast",
                        help="Continue testing after failures")
    parser.add_argument("-j", "--jobs", type=int, default=0,
                        help="Parallel worker count (default: auto)")
    parser.add_argument("-t", "--timeout", type=float, default=10.0,
                        help="Per-test timeout in seconds (default: 10)")
    parser.add_argument("--ignore-list", metavar="FILE",
                        help="File with ELF basenames to skip (one per line, # comments allowed). "
                             "A warning is printed for any ignored test that passes.")
    args = parser.parse_args()

    emu, elf_dir = get_project_paths()

    # Load ignore list if provided
    ignore_set: set[str] = set()
    if args.ignore_list:
        ignore_path = Path(args.ignore_list)
        if not ignore_path.is_file():
            print(c(Colors.RED, f"Error: ignore list not found: {ignore_path}"))
            sys.exit(1)
        for raw in ignore_path.read_text().splitlines():
            entry = raw.split("#", 1)[0].strip()
            if entry:
                ignore_set.add(entry)

    if not emu.is_file():
        print(c(Colors.RED, f"Error: emulator not found at {emu}"))
        print("Please build the project first: cmake --build build")
        sys.exit(1)

    if not elf_dir.is_dir():
        print(c(Colors.RED, f"Error: ELF directory not found at {elf_dir}"))
        print("Please run: cmake --build build --target link-tests")
        sys.exit(1)

    # Collect ELF files
    if args.file:
        p = Path(args.file)
        files = [p if p.is_absolute() else elf_dir / p.name]
    else:
        files = sorted(elf_dir.glob("*.elf"))

    if args.filter:
        files = [f for f in files if fnmatch.fnmatch(f.name, args.filter)]

    # Split into active and ignored
    ignored_files = [f for f in files if f.name in ignore_set]
    active_files  = [f for f in files if f.name not in ignore_set]

    if not active_files and not ignored_files:
        print("No ELF files found.")
        sys.exit(0)

    print("=== Blackfin JIT Emulator Test ===\n")
    print(f"emu:     {emu}")
    print(f"elfs:    {elf_dir}")
    print(f"tests:   {len(active_files)}  (+ {len(ignored_files)} ignored)")
    print(f"timeout: {args.timeout}s")
    print()

    summary = Summary(total=len(active_files), ignored=len(ignored_files))
    max_workers = args.jobs or min(max(len(active_files) + len(ignored_files), 1), os.cpu_count() or 4)

    # Build a partial run_one with emu path baked in (avoids re-deriving inside worker)
    def _run(elf_path: Path) -> EmuResult:
        t0 = time.monotonic()
        try:
            proc = subprocess.run(
                [str(emu), str(elf_path), "-O", "0"],
                capture_output=True,
                text=True,
                timeout=args.timeout,
            )
            duration = time.monotonic() - t0
            rc = proc.returncode
            if rc == 0:
                status = "pass"
            elif rc == 1:
                status = "fail"
            else:
                status = "error"
            return EmuResult(elf_path.name, status, rc, proc.stdout, proc.stderr, duration)
        except subprocess.TimeoutExpired as exc:
            duration = time.monotonic() - t0
            stdout = (exc.stdout or b"").decode(errors="replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
            stderr = (exc.stderr or b"").decode(errors="replace") if isinstance(exc.stderr, bytes) else (exc.stderr or "")
            return EmuResult(elf_path.name, "timeout", -1, stdout, stderr, duration)

    stopped_early = False
    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        future_to_file = {executor.submit(_run, f): f for f in active_files if f.is_file()}
        for future in as_completed(future_to_file):
            r = future.result()
            print_result(r, args.verbose)

            if r.status == "pass":
                summary.passed += 1
            elif r.status == "fail":
                summary.failed += 1
                summary.failures.append(r.name)
            elif r.status == "timeout":
                summary.timed_out += 1
                summary.failures.append(r.name)
            else:
                summary.errors += 1
                summary.failures.append(r.name)

            if args.fail_fast and r.status != "pass":
                for f in future_to_file:
                    f.cancel()
                print(c(Colors.RED, "\nStopping at first failure (use --no-fail-fast to continue)"))
                stopped_early = True
                break

    # Probe ignored tests — warn if any unexpectedly pass
    if ignored_files:
        with ThreadPoolExecutor(max_workers=max_workers) as executor:
            ignored_futures = {executor.submit(_run, f): f for f in ignored_files if f.is_file()}
            for future in as_completed(ignored_futures):
                r = future.result()
                if r.status == "pass":
                    summary.ignored_but_passing.append(r.name)

    if summary.ignored_but_passing:
        print(c(Colors.YELLOW, "\n=== Ignored tests that are now PASSING (consider removing from ignore list) ==="))
        for name in sorted(summary.ignored_but_passing):
            print(c(Colors.YELLOW, f"  [WARN] {name}"))

    # Summary
    print(f"\n=== Summary ===")
    print(f"Total:    {summary.total}")
    print(f"Passed:   {c(Colors.GREEN, str(summary.passed))}")
    print(f"Failed:   {c(Colors.RED, str(summary.failed))}")
    print(f"Timeout:  {c(Colors.YELLOW, str(summary.timed_out))}")
    print(f"Error:    {c(Colors.MAGENTA, str(summary.errors))}")
    print(f"Ignored:  {summary.ignored}" + (f"  ({len(summary.ignored_but_passing)} now passing)" if summary.ignored_but_passing else ""))
    if stopped_early:
        print(c(Colors.YELLOW, "(stopped early — some tests not run)"))
    print()

    any_bad = summary.failed + summary.timed_out + summary.errors
    sys.exit(1 if any_bad > 0 else 0)


if __name__ == "__main__":
    main()
