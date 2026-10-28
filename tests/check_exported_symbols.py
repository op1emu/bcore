#!/usr/bin/env python3
"""
Every symbol bcore's static libraries export names only bcore's own names,
the standard library's and LLVM's: a host that links them may define any
global name of its own.

A global type of bcore's would clash with a host's of the same name: the
linker keeps one copy of each weak member of a template instantiated on it
(std::unordered_map<unsigned, BBInfo>) or of its vtable, typeinfo and inline
functions, whichever layout the host's has, and says nothing (bcore PR #35:
a host's 20-byte `struct Access` made a pass's std::vector<Access> throw).
A global function, extern "C" or not, is a duplicate definition or, worse,
the host's version.

  check_exported_symbols.py --nm llvm-nm libbcore-runtime.a libbcore-lift.a libbcore-engine.a libbfin-core.a

Each demangled symbol's names (a qualified name, or a name in its template
arguments or parameters) must begin with `bcore`, `std`, `llvm` or
`__gnu_cxx`, or be a C++ keyword, a word of the demangler's or a name
reserved to the implementation (__x, _X). Besides, the observability API's
types are global and prefixed: Bcore*.
"""

import argparse
import re
import subprocess
import sys
from typing import List

NAMESPACES = {"bcore", "std", "llvm", "__gnu_cxx", "__cxxabiv1"}

# The public profile API (include/bcore_profile.h) is global, prefixed.
PREFIXED = re.compile(r"Bcore[A-Z]\w*$")

# A reserved name (__x, _X: the implementation's, which no host may
# define), as the compiler's own helpers are: clang's
# __clang_call_terminate, ___clang_call_terminate on Mach-O.
RESERVED = re.compile(r"_[_A-Z]")

# ELF's reference to the personality routine of code that may throw.
PERSONALITY = "DW.ref.__gxx_personality_v0"

KEYWORDS = {
    "void", "bool", "char", "signed", "unsigned", "short", "int", "long", "float", "double",
    "wchar_t", "char8_t", "char16_t", "char32_t", "__int128", "decltype", "auto", "const",
    "volatile", "true", "false", "nullptr", "operator", "new", "delete", "sizeof", "alignof",
    "noexcept", "restrict", "__restrict",
}

# The standard library's names the demangler prints without std:: in an
# expression within a symbol's template arguments (LLVM 23's
# PassManager::addPass: enable_if<!is_same_v<...>>).
STD_IN_EXPRESSIONS = {"is_same_v"}

# The demangler's words around a name ("vtable for X") and in it.
DEMANGLER = re.compile(
    r"\b(?:vtable|VTT|typeinfo name|typeinfo|guard variable|construction vtable|"
    r"non-virtual thunk|virtual thunk|covariant return thunk|reference temporary #\d+|"
    r"TLS init function|TLS wrapper function|transaction clone)\s+(?:for|to)\s+"
    r"|-in-"
    r"|\[(?:abi|clone)[^\]]*\]"
    r"|\{lambda|'lambda\d*'|'unnamed\d*'|\{unnamed type#\d+\}|\(anonymous namespace\)|#\d+\}"
)

# Not a literal's suffix (25u, 1ul).
NAME = re.compile(r"(?<![\w.])~?[A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)*")


def foreign_names(symbol: str) -> List[str]:
    """The names in a demangled symbol that are none of the library's,
    the standard library's or LLVM's."""
    text = DEMANGLER.sub(" ", symbol)
    found = []
    for m in NAME.finditer(text):
        # A component of a name already seen (`b` in `a::b<...>::c`) comes
        # with its own match only after a template's arguments close.
        if m.start() > 0 and text[m.start() - 1] == ":":
            continue
        name = m.group(0)
        head = name.split("::")[0]
        if (head in NAMESPACES or name in KEYWORDS or name in STD_IN_EXPRESSIONS or PREFIXED.match(head)
                or RESERVED.match(head)):
            continue
        found.append(name)
    return found


def exported_symbols(nm: str, libraries: List[str]) -> List[str]:
    out = subprocess.run([nm, "--demangle", "--defined-only", "--extern-only", "--just-symbol-name", *libraries],
                         check=True, capture_output=True, text=True).stdout
    symbols = set()
    for line in out.splitlines():
        line = line.strip()
        # llvm-nm heads each archive member's symbols with "member.o:".
        if not line or line.endswith(":") or line == PERSONALITY:
            continue
        symbols.add(line)
    return sorted(symbols)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nm", default="llvm-nm", help="llvm-nm (it demangles Mach-O's names as ELF's)")
    ap.add_argument("libraries", nargs="+")
    args = ap.parse_args()
    bad = [(s, foreign_names(s)) for s in exported_symbols(args.nm, args.libraries)]
    bad = [(s, names) for s, names in bad if names]
    for symbol, names in bad:
        print(f"{', '.join(sorted(set(names)))}: {symbol}")
    if bad:
        print(f"FAIL: {len(bad)} exported symbols name a global of bcore's (put it in namespace bcore)",
              file=sys.stderr)
        return 1
    print("every exported symbol is bcore's, the standard library's or LLVM's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
