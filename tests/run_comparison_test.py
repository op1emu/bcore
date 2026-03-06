#!/usr/bin/env python3
"""
Comparison test script for bfin disassembler.
Compares output from disasm tool against bfin-elf-objdump reference.
"""

import argparse
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Optional


# ANSI colors
class Colors:
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    NC = '\033[0m'


@dataclass
class TestResult:
    total: int = 0
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    unknown_instructions: int = 0


def _find_objdump(build_dir: Path) -> Path:
    """Resolve bfin-elf-objdump path.

    Search order:
    1. CMake-generated build/test_config.py (set by cmake/BfinToolchain.cmake)
    2. BFIN_OBJDUMP environment variable
    3. Legacy hardcoded location (backwards compatibility)
    4. PATH search via shutil.which
    """
    import importlib.util
    import shutil

    # 1. CMake-generated config
    config_file = build_dir / "test_config.py"
    if config_file.is_file():
        spec = importlib.util.spec_from_file_location("test_config", config_file)
        cfg = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cfg)
        cmake_path = getattr(cfg, "BFIN_OBJDUMP", None)
        # Skip CMake NOTFOUND sentinels and paths that don't exist on this machine
        if cmake_path and "NOTFOUND" not in cmake_path:
            p = Path(cmake_path)
            if p.is_file():
                return p

    # 2. Environment variable
    env_path = os.environ.get("BFIN_OBJDUMP")
    if env_path:
        return Path(env_path)

    # 3. Legacy location (unchanged from original, for compatibility)
    legacy = Path.home() / "toolchains/bfin-elf/bin/bfin-elf-objdump"
    if legacy.is_file():
        return legacy

    # 4. PATH search
    found = shutil.which("bfin-elf-objdump")
    if found:
        return Path(found)

    # Return a non-existent path; the is_file() check in main() will catch it.
    return Path("bfin-elf-objdump")


def get_project_paths() -> tuple[Path, Path, Path, Path]:
    """Get project directory paths."""
    script_dir = Path(__file__).parent.resolve()
    project_root = script_dir.parent
    build_dir = project_root / "build"

    disasm = build_dir / "disasm"
    objdump = _find_objdump(build_dir)
    test_objects = build_dir / "test_objects"
    results_dir = build_dir / "test_results"

    return disasm, objdump, test_objects, results_dir


def run_command(cmd: list[str], capture_output: bool = True) -> Optional[str]:
    """Run a command and return stdout, or None on failure."""
    try:
        result = subprocess.run(cmd, capture_output=capture_output, text=True, timeout=30)
        if result.returncode == 0:
            return result.stdout
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
    return None


def extract_disasm_map(output: str) -> tuple[Dict[str, str], list[tuple[str, str]]]:
    """
    Extract address->instruction map from disasm output.
    Format: "00000000: 0025  EMUEXCPT" -> {"0": "emuexcpt"}
    Format: "00000004: 17e1  CLI R7" -> {"4": "cli r7"}
    Returns: (insn_map, unknown_list) where unknown_list is [(addr, hex_bytes), ...]
    """
    insn_map = {}
    unknown_list = []
    for line in output.splitlines():
        if line.startswith(".text"):
            continue

        if "<unknown>" in line:
            # Extract address and hex bytes from unknown instruction
            match = re.match(r'^([0-9a-f]+):\s*([0-9a-f]+)', line)
            if match:
                addr = match.group(1)
                hex_bytes = match.group(2)
                unknown_list.append((addr, hex_bytes))
            continue

        # Capture full instruction (everything after hex bytes)
        match = re.match(r'^([0-9a-f]+):\s*[0-9a-f]+\s+(.+)$', line)
        if match:
            addr = match.group(1).lstrip('0') or '0'
            instruction = match.group(2).strip().lower()
            insn_map[addr] = instruction

    return insn_map, unknown_list


def extract_objdump_map(output: str) -> Dict[str, str]:
    """
    Extract address->instruction map from objdump output.
    Format: "   0:	25 00       	EMUEXCPT;" -> {"0": "emuexcpt"}
    Format: "   4:	17 e1       	CLI R7;" -> {"4": "cli r7"}
    Format: "   8:	00 00       	NOP;  # comment" -> {"8": "nop"}
    Format: "   c:	14 e0       	RTS;  /* comment */" -> {"c": "rts"}
    Format: "  10:	20 e3 74 00 	IF !CC JUMP 0x74 <__fail+0x20>;" -> {"10": "if !cc jump 0x74"}
    """
    insn_map = {}
    for line in output.splitlines():
        # Match lines with address: hex bytes  instruction;
        match = re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+)+)\s*(.+?);?\s*$', line)
        if match:
            addr = match.group(1).lstrip('0') or '0'
            instruction = match.group(3).rstrip(';').strip()

            # Strip symbols in angle brackets <symbol+offset>
            instruction = re.sub(r'<[^>]+>', '', instruction).strip()

            # Strip C-style block comments /* ... */
            instruction = re.sub(r'/\*.*?\*/', '', instruction).strip()

            # Strip line comments (both # and //)
            for comment_marker in ['#', '//']:
                if comment_marker in instruction:
                    instruction = instruction.split(comment_marker)[0].strip()

            # Normalize multiple spaces to single space
            instruction = re.sub(r'\s+', ' ', instruction).strip()

            # Clean up spaces around punctuation (after stripping symbols)
            instruction = re.sub(r'\s*,\s*', ', ', instruction)  # "x ,y" -> "x, y"
            instruction = re.sub(r'\(\s+', '(', instruction)     # "( x" -> "(x"
            instruction = re.sub(r'\s+\)', ')', instruction)     # "x )" -> "x)"
            instruction = instruction.strip()

            # Final cleanup: remove any trailing semicolons
            instruction = instruction.rstrip(';').strip()

            instruction = instruction.lower()
            # Skip if instruction looks like it starts with a number (parse error)
            if instruction and not instruction[0].isdigit():
                insn_map[addr] = instruction

    return insn_map


def run_test(obj_file: Path, disasm: Path, objdump: Path, results_dir: Path,
             verbose: bool = False) -> tuple[Optional[bool], int]:
    """
    Run comparison test on a single object file.
    Returns: (test_result, unknown_count) where test_result is True=pass, False=fail, None=skip
    """
    basename = obj_file.stem

    # Run disasm and objdump in parallel
    with ThreadPoolExecutor(max_workers=2) as executor:
        f_disasm = executor.submit(run_command, [str(disasm), str(obj_file)])
        f_objdump = executor.submit(run_command, [str(objdump), "-d", str(obj_file)])
    disasm_output = f_disasm.result()
    objdump_output = f_objdump.result()

    if disasm_output is None:
        print(f"{Colors.YELLOW}[SKIP]{Colors.NC} {basename} - disasm failed")
        return None, 0
    if objdump_output is None:
        print(f"{Colors.YELLOW}[SKIP]{Colors.NC} {basename} - objdump failed")
        return None, 0

    # Save outputs for debugging
    results_dir.mkdir(parents=True, exist_ok=True)
    (results_dir / f"{basename}.disasm").write_text(disasm_output)
    (results_dir / f"{basename}.objdump").write_text(objdump_output)

    # Extract instruction maps
    disasm_map, unknown_list = extract_disasm_map(disasm_output)
    objdump_map = extract_objdump_map(objdump_output)

    total_known = len(disasm_map)
    total_objdump = len(objdump_map)
    total_unknown = len(unknown_list)

    # Print unknown instructions if verbose
    if verbose and total_unknown > 0:
        print(f"\n  {Colors.YELLOW}Unknown instructions in {basename}:{Colors.NC}")
        for addr, hex_bytes in unknown_list:
            # Look up what objdump says about this address
            addr_key = addr.lstrip('0') or '0'
            ref_mnemonic = objdump_map.get(addr_key, '???')
            print(f"    0x{addr}: {hex_bytes}  (objdump: {ref_mnemonic})")
        print()

    if total_known == 0:
        print(f"{Colors.YELLOW}[SKIP]{Colors.NC} {basename} - no recognized instructions (0/{total_objdump}, {total_unknown} unknown)")
        return None, total_unknown

    # Compare instructions at matching addresses
    matched = 0
    mismatched = 0
    mid_insn = 0

    for addr, mnemonic in disasm_map.items():
        ref_mnemonic = objdump_map.get(addr)
        if ref_mnemonic is None:
            mid_insn += 1
            if verbose:
                print(f"  Address 0x{addr}: disasm='{mnemonic}' (not in objdump - likely mid-instruction)")
        elif mnemonic == ref_mnemonic:
            matched += 1
        else:
            mismatched += 1
            if verbose:
                print(f"  Mismatch at 0x{addr}: disasm='{mnemonic}' objdump='{ref_mnemonic}'")

    # Calculate stats
    valid_known = total_known - mid_insn
    pct = (matched * 100 // valid_known) if valid_known > 0 else 0

    if matched == valid_known and valid_known > 0:
        status = f"{Colors.GREEN}[PASS]{Colors.NC} {basename} ({matched}/{valid_known} match"
        if mid_insn > 0:
            status += f", {mid_insn} mid-insn"
        if total_unknown > 0:
            status += f", {Colors.YELLOW}{total_unknown} unknown{Colors.NC}"
        status += f", {total_objdump} total)"
        print(status)
        return True, total_unknown
    else:
        status = f"{Colors.RED}[FAIL]{Colors.NC} {basename} - {matched}/{valid_known} match ({pct}%)"
        if mismatched > 0:
            status += f", {mismatched} mismatch"
        if mid_insn > 0:
            status += f", {mid_insn} mid-insn"
        if total_unknown > 0:
            status += f", {Colors.YELLOW}{total_unknown} unknown{Colors.NC}"
        print(status)
        return False, total_unknown


def show_details(obj_file: Path, disasm: Path, objdump: Path):
    """Show detailed side-by-side comparison for a specific file."""
    print(f"=== Detailed comparison for {obj_file.stem} ===\n")

    # Get outputs
    disasm_output = run_command([str(disasm), str(obj_file)])
    objdump_output = run_command([str(objdump), "-d", str(obj_file)])

    if not disasm_output or not objdump_output:
        print("Error: Could not get disassembly output")
        return

    # Parse objdump output to get both hex bytes and instructions
    objdump_lines = {}
    for line in objdump_output.splitlines():
        match = re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+)+)\s*(.+)$', line)
        if match:
            addr = match.group(1).lstrip('0') or '0'
            hex_bytes = match.group(2).strip()
            objdump_lines[addr] = hex_bytes

    # Parse disasm output
    disasm_map, unknown_list = extract_disasm_map(disasm_output)
    objdump_map = extract_objdump_map(objdump_output)

    # Create a set of unknown addresses for quick lookup
    unknown_addrs = {addr.lstrip('0') or '0' for addr, _ in unknown_list}

    # Get all addresses from objdump (sorted)
    all_addrs = sorted(objdump_map.keys(), key=lambda x: int(x, 16) if x != '0' else 0)

    # First, calculate total statistics for ALL instructions
    total_matched = 0
    total_mismatched = 0
    total_unknown = 0

    for addr in all_addrs:
        objdump_insn = objdump_map[addr]
        disasm_insn = disasm_map.get(addr)

        # Determine status
        if addr in unknown_addrs:
            total_unknown += 1
        elif disasm_insn is None:
            total_unknown += 1
        elif disasm_insn == objdump_insn:
            total_matched += 1
        else:
            total_mismatched += 1

    # Print legend
    print(f"Color legend: {Colors.GREEN}Green = Match{Colors.NC}, "
          f"{Colors.RED}Red = Mismatch{Colors.NC}, "
          f"{Colors.YELLOW}Yellow = Unknown{Colors.NC}\n")

    # Print header
    print(f"{'Address':>10} {'Hex Bytes':<12} {'Objdump (reference)':<30} {'Disasm (ours)':<30}")
    print("=" * 82)

    for addr in all_addrs:
        objdump_insn = objdump_map[addr]
        disasm_insn = disasm_map.get(addr)
        hex_bytes = objdump_lines.get(addr, '')

        # Determine color
        if addr in unknown_addrs:
            color = Colors.YELLOW
        elif disasm_insn is None:
            color = Colors.YELLOW
        elif disasm_insn == objdump_insn:
            color = Colors.GREEN
        else:
            color = Colors.RED

        # Format address with hex prefix (right-aligned)
        addr_formatted = f"{'0x' + addr:>10}"

        # Format instructions
        hex_bytes_str = f"{hex_bytes:<12}"
        objdump_str = f"{objdump_insn:<30}"
        disasm_str = f"{color}{(disasm_insn or '<unknown>'):<30}{Colors.NC}"

        print(f"{addr_formatted} {hex_bytes_str} {objdump_str} {disasm_str}")

    # Print summary
    print("=" * 82)

    # Note: total_unknown only counts unknowns at valid objdump addresses
    # Some <unknown> from disasm may be at addresses not in objdump (mid-instruction)
    unknown_count_note = ""
    if len(unknown_list) != total_unknown:
        unknown_count_note = f" ({len(unknown_list)} total in disasm, {len(unknown_list) - total_unknown} at invalid addresses)"

    print(f"\nSummary (all instructions): {Colors.GREEN}{total_matched} matched{Colors.NC}, "
          f"{Colors.RED}{total_mismatched} mismatched{Colors.NC}, "
          f"{Colors.YELLOW}{total_unknown} unknown{Colors.NC}{unknown_count_note}\n")


def main():
    parser = argparse.ArgumentParser(description="Blackfin disassembler comparison test")
    parser.add_argument("-v", "--verbose", action="store_true", help="Show verbose output")
    parser.add_argument("-f", "--file", help="Test only a specific file")
    parser.add_argument("-d", "--details", help="Show detailed comparison for a file")
    parser.add_argument("-F", "--fail-fast", action="store_true", default=True,
                        dest="fail_fast", help="Stop at first failure (default: enabled)")
    parser.add_argument("--no-fail-fast", action="store_false",
                        dest="fail_fast", help="Continue testing after failures")
    parser.add_argument("-j", "--jobs", type=int, default=0,
                        help="Parallel worker count (default: auto = CPU count)")
    args = parser.parse_args()

    disasm, objdump, test_objects, results_dir = get_project_paths()

    # Check prerequisites
    if not disasm.is_file():
        print(f"{Colors.RED}Error: disasm not found at {disasm}{Colors.NC}")
        print("Please build the project first: cd build && cmake .. && make")
        sys.exit(1)

    if not objdump.is_file():
        print(f"{Colors.RED}Error: bfin-elf-objdump not found at {objdump}{Colors.NC}")
        sys.exit(1)

    if not test_objects.is_dir():
        print(f"{Colors.RED}Error: test objects directory not found at {test_objects}{Colors.NC}")
        print("Please run: cd build && make assemble-tests")
        sys.exit(1)

    # Handle --details
    if args.details:
        show_details(test_objects / args.details, disasm, objdump)
        sys.exit(0)

    print("=== Blackfin Disassembler Comparison Test ===\n")
    print(f"disasm:   {disasm}")
    print(f"objdump:  {objdump}")
    print(f"objects:  {test_objects}\n")

    result = TestResult()

    # Get files to test
    if args.file:
        files = [test_objects / args.file]
    else:
        files = sorted(test_objects.glob("*.o"))

    # Run tests in parallel
    max_workers = args.jobs or min(max(len(files), 1), os.cpu_count() or 4)
    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        future_to_file = {
            executor.submit(run_test, f, disasm, objdump, results_dir, args.verbose): f
            for f in files if f.is_file()
        }
        result.total = len(future_to_file)
        for future in as_completed(future_to_file):
            test_result, unknown_count = future.result()
            result.unknown_instructions += unknown_count
            if test_result is True:
                result.passed += 1
            elif test_result is False:
                result.failed += 1
                if args.fail_fast:
                    for f in future_to_file:
                        f.cancel()
                    print(f"\n{Colors.RED}Stopping at first failure (use --no-fail-fast to continue){Colors.NC}")
                    break
            else:
                result.skipped += 1

    # Print summary
    print(f"\n=== Summary ===")
    print(f"Total:   {result.total}")
    print(f"Passed:  {Colors.GREEN}{result.passed}{Colors.NC}")
    print(f"Failed:  {Colors.RED}{result.failed}{Colors.NC}")
    print(f"Skipped: {Colors.YELLOW}{result.skipped}{Colors.NC}")
    print(f"Unknown instructions: {Colors.YELLOW}{result.unknown_instructions}{Colors.NC}\n")

    sys.exit(1 if result.failed > 0 else 0)


if __name__ == "__main__":
    main()
