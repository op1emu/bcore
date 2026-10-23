# Experiences

## Design Principle: decode_ functions only signal, never emit on failure
`decode_` functions that reject an instruction should only `return false`. All "illegal"
output is the caller's responsibility. This keeps decode_ functions free of side effects
on the failure path and centralises the output policy in `decodeInstruction`.

`decodeInstruction16` propagates `false` from a visitor veto (without calling
`decode_unknown_16`), so the parallel block can distinguish "no pattern matched" (which
`decodeInstruction16` handles internally via `decode_unknown_16`) from "pattern matched
but illegal in this context" (which the parallel block handles by calling
`decode_unknown_16` itself after the false return).

## Blackfin 40-bit accumulator: AX is a sign extension of AW
The `A0 = Rs` / `A1 = Rs` instructions (dsp32alu ACC_LOAD) automatically sign-extend
the loaded 32-bit value into the 8-bit extension register:

```c
// From refs/bfin-sim.h SET_AREG32 macro:
AX[n] = -(AW[n] >> 31);   // 0xFF if AW negative, 0x00 if positive
```

In LLVM IR: `ax = CreateAShr(rs, i32(31))` — arithmetic right shift fills with sign bit.

Without this, 40-bit accumulator comparisons (`CC = A0 == A1`, etc.) produce wrong
results for any negative register value, because the 40-bit value is positive instead
of the correct signed extension.

## Blackfin libgloss syscall convention (EXCPT 0)
Blackfin libgloss does NOT pass syscall arguments in R0/R1/R2 directly. Instead:
- **P0** = syscall number (1 = exit, 5 = write)
- **R0** = pointer to a `_params[]` array in `.data` section holding the actual args

For `write(1, buf, count)`:  `_params = {fd=1, buf_addr, count}`
For `exit(rc)`:              `_params = {rc}`

Read args via `mem_read32(mem, base, args_ptr + offset)`.

This explains why exit codes and write calls were wrong when R0/R1/R2 were read directly.


## JIT symbol resolution: use absoluteSymbols, not DynamicLibrarySearchGenerator

LLVM ORC JIT's `DynamicLibrarySearchGenerator::GetForCurrentProcess()` relies on symbols
being exported from the host executable (`-rdynamic` / `ENABLE_EXPORTS`). This is fragile:
LTO or the linker may strip symbols, and `inline` functions in headers have no symbol at all.

**Reliable approach**: register all extern "C" functions explicitly with `absoluteSymbols`:

```cpp
llvm::orc::SymbolMap symbols;
symbols[es.intern("mem_read32")] = JITEvaluatedSymbol(
    pointerToJITTargetAddress(&mem_read32),
    JITSymbolFlags::Exported | JITSymbolFlags::Callable);
jd.define(absoluteSymbols(symbols));
```

Corollary: any function called from JIT'd code **must not be `inline`**. Move them to a `.cpp`
file with `extern "C"` linkage. This bit us with `cpu_astat_compose` / `cpu_astat_decompose`
which were `inline` in `cpu_state.h` — the linker never emitted a symbol for them.


## pcrel24 is 24-bit sign-extended then shifted, not 25-bit

The CALLa instruction has a 24-bit immediate field. The reference computes:

```c
pcrel24(x) = SIGNEXTEND(x, 24) << 1   // sign-extend 24 bits, then shift left by 1
```

This is **not** a 25-bit sign extension. Using `signextend<25>` on a 24-bit value leaves
bit 24 as zero (since the decoder only extracts 24 bits), producing a large positive offset
instead of a small negative one. Always match the sign-extension width to the actual
field width, then apply the scale factor separately.

Format table from `refs/bfin-sim.c`:
| Name     | Bits | Scale | Formula                        |
|----------|------|-------|--------------------------------|
| pcrel10  | 10   | 1     | `SIGNEXTEND(x, 10) << 1`      |
| pcrel12  | 12   | 1     | `SIGNEXTEND(x, 12) << 1`      |
| pcrel24  | 24   | 1     | `SIGNEXTEND(x, 24) << 1`      |


## dsp32alu ACC_LOAD: aop field maps A0 (0,1) vs A1 (2,3)

The `sop` field in dsp32alu sopcde=9 instructions determines which accumulator is targeted:

| sop | s | Meaning     |
|-----|---|-------------|
| 00  | 1 | A0 = Rs     |
| 00  | 0 | A0.W / A0.X |
| 01  | 0 | A0.X = Rs   |
| 10  | 1 | A1 = Rs     |
| 10  | 0 | A1.W / A1.X |
| 11  | 0 | A1.X = Rs   |

Sharing implementations between A0 and A1 variants (e.g., AOP2 delegating to AOP0) is a
bug — each must write to its own accumulator (aw[0]/ax[0] vs aw[1]/ax[1]).


## testutils.inc syscall macros (test infrastructure)
The `write`, `exit`, `pass`, `fail` macros in `build/bfin_testsuite_src/testsuite/testutils.inc` (fetched from https://github.com/op1emu/bfin_sim) compile to
Blackfin libgloss syscall sequences. Key points:
- `pass` → CALL __pass → write "pass\n" then exit(0)
- `fail` → CALL __fail → write "fail at PC=0x..." then exit(1)
- `__fail` uses RETS−4 to determine which CALL site triggered the failure and
  writes the PC as a hex string — useful for pinpointing which test case failed

## dsp32alu ADDADD: AN uses bit-15 of the 16-bit result, not 32-bit sign

When implementing dual 16-bit halfword adds (`Rd = Rs +|+ Rt`), the `AN` (negative) flag
must be set based on bit 15 of each **16-bit** result, not the sign of the 32-bit
sign-extended sum.

**Wrong**:
```cpp
auto* an_l = builder_.CreateICmpSLT(sum_l, builder_.getInt32(0));
```
`sum_l = 1 + 32767 = 32768` is positive in 32 bits → AN=0, but the 16-bit result `0x8000`
has bit 15 set → AN should be 1.

**Correct**:
```cpp
// t1 = sum_l & 0xFFFF  (already masked 16-bit result)
auto* an_l = builder_.CreateICmpSGT(t1, builder_.getInt32(0x7FFF));
```

The reference `add16()` computes: `bs16 result = (bs16)a + (bs16)b; *neg |= result < 0`
where `result < 0` is a 16-bit signed comparison — i.e., bit 15 of the truncated result.

This principle applies to all Blackfin instructions that set ASTAT flags based on
16-bit arithmetic results.


## emit_mac_common: halfword extraction requires rnd16/trunc16 + saturate, not raw aw bits

The default halfword extraction from a 40-bit accumulator is **not** `aw & 0xFFFF`. The
reference `extract_mult` applies rounding and saturation before returning a 16-bit result.

All mmod cases for halfword (P=0) output:

| mmod | value | Operation |
|------|-------|-----------|
| 0, W32, IH | 0/3/11 | `saturate_s16(rnd16(acc))` |
| S2RND | 1 | `saturate_s16(rnd16(acc << 1))` |
| T | 2 | `saturate_s16(trunc16(acc))` |
| FU | 4 | MM=0: `saturate_u16(rnd16(acc))`, MM=1: `saturate_s16(rnd16(acc))` |
| TFU | 6 | MM=0: `saturate_u16(trunc16(acc))`, MM=1: `saturate_s16(trunc16(acc))` |
| IS | 8 | `saturate_s16(acc)` — no rounding |
| ISS2 | 9 | `saturate_s16(acc << 1)` — no rounding |
| IU | 12 | MM=0: `saturate_u16(acc)`, MM=1: `saturate_s16(acc)` |

`rnd16`: round-to-nearest-even on bits [15:0], then shift right 16 (preserve sign bits [63:48]).
`trunc16`: pure truncation — shift right 16 (preserve sign bits [63:48]), no rounding.
`saturate_u16`: clamp to [0, 0xFFFF] by **unsigned** comparison — large (wrapping) values also clamp.
`saturate_s16`: clamp to [-32768, 32767] by **signed** comparison.

The FU/TFU accumulator saturation uses **unsigned** range [0, 0xFFFFFFFFFF] when MM=0
(not the standard signed 40-bit range). IS/ISS2 and signed modes use signed range.


## emit_dsp32mac: V/VS/V_COPY flags come from extract_mult overflow, not acc saturation

V is set when the 16-bit (or 32-bit) extraction from the accumulator overflows the target
type — e.g., a pre-loaded A0 value that is large enough that `rnd16(acc)` > 0xFFFF for FU
mode. Accumulator saturation (AV0/AV1) is a separate flag.

`emit_mac_common` must return an overflow i32 `v_out`. `emit_dsp32mac` ORs `v0 | v1` and
writes to V (via `store_v`, which also sets V_COPY) and sticky-ORs into VS.

The reference code path: `if (sat) ... extract_mult(nosat_acc, ...overflow)` — for halfword
mode, V comes from `extract_mult`, not directly from `sat`.


## dsp32mult vs dsp32mac: key differences

- MULT = MAC with `op0=0, op1=0` (assignment, not accumulation) — delegate to `emit_dsp32mac`
- A0 always uses `MM=0`; only A1 uses the instruction's `MM` field
- MULT does NOT update the accumulator registers (A0/A1 contents unchanged) — set `update_acc=false`
- AZ/AN flags are NOT set by MULT (op is never 3); only V/VS from extraction overflow


## ALU2op_ADD_SHIFT1/2: v_internal accumulates from add AND shift overflow

`Rd = (Rd + Rs) << N` uses an internal `v_internal` flag that tracks whether **any**
overflow occurred during either the add step or any shift step.

The reference `add_and_shift()` in `refs/bfin-sim.c`:
1. Initialises `v_internal = 0`
2. Calls `add32()` which does `v_internal |= signed_overflow` internally
3. Each shift step: if `bits[31:30]` of the pre-shift value differ (i.e., top two bits
   are `01` or `10`), sets `v_internal = 1`
4. Final: `AZ = (result==0)`, `AN = (result<0)`, `V = v_internal`, `if V: VS = 1` (sticky)

The final `V = v_internal` **overwrites** any V set by add32 — but since add32 also
wrote to v_internal, add overflow is still captured in V.

### LLVM IR overflow detection

**Signed add overflow**: `(a XOR sum)[bit31] AND (b XOR sum)[bit31]`
(both operands have same sign, result has different sign)

**Per-shift overflow**: `x = (v >> 30) & 3; overflow = (x==1 || x==2)` which equals
`(x & 1) XOR (x >> 1)` — detects when the top two bits differ.

### Pitfall
Implementing this without the add-overflow contribution causes test case 4 of
`add_shift.S` to fail: `(0x80000000 + 0x80000000) << 1` = 0x0 with V=1, but without
add32's contribution to v_internal, the shift step sees top2=0 (no shift overflow) and
incorrectly sets V=0.


## SIGNBITS on 40-bit accumulator: prefer inline IR over extern "C" helper

`SIGNBITS A0` / `SIGNBITS A1` count the redundant sign bits of the 40-bit accumulator,
subtract 8 for the extension byte, and write the result to `Rd.L`.

### IR formula

For a 40-bit value sign-extended to i64:

```
sign_ext  = ashr acc, 63          ; all-1s if negative, 0 if positive
normalized = xor acc, sign_ext    ; flip bits → always non-negative
result    = ctlz(normalized) - 33 ; 33 = 24 (unused bits) + 1 (sign bit) + 8 (ext)
```

`ctlz` with `is_zero_poison=false` returns 64 for zero, giving `64-33=31` for both
`acc=0` and `acc=-1` — both have 31 redundant sign bits in 40-bit context. No special
case needed.

### IR intrinsic in LLVM C++ API

```cpp
auto* ctlz_fn = Intrinsic::getDeclaration(module_, Intrinsic::ctlz,
                                          {builder_.getInt64Ty()});
auto* leading = builder_.CreateCall(ctlz_fn, {normalized, builder_.getFalse()});
```

Requires `#include <llvm/IR/Intrinsics.h>`.

### Why not extern "C"?

An extern "C" helper works but adds runtime call overhead and requires registration in
`jit_engine.cpp`'s `absoluteSymbols` table. Inline IR lets the optimizer fold constants
(e.g., `SIGNBITS A0` after `A0 = R0` with a known R0 value) and avoids the call entirely.


## COMPI2opD_ADD (Rd += imm7): must emit full ASTAT flags via emit_flags_arith

`decode_COMPI2opD_ADD` is a 16-bit add-immediate instruction. The arithmetic produces the
correct result but **all ASTAT flags remain zero** if `emit_flags_arith` is not called.
`dbga` assertions that test AZ/AN/V/AC0 after `Rd += imm7` will therefore always fail.

The full flag computation matches the `add32` model in `refs/bfin-sim.c`:

```cpp
// Signed overflow: (src_sign XOR result_sign) AND (imm_sign XOR result_sign)
auto* flgs = CreateLShr(rd,      31);
auto* flgo = CreateLShr(imm_val, 31);
auto* flgn = CreateLShr(result,  31);
auto* v_flag = ZExt(And(Xor(flgs,flgn), Xor(flgo,flgn)), i32);

// Unsigned carry: ~rd < imm_val (unsigned comparison)
auto* ac0 = ZExt(ICmpULT(CreateNot(rd), imm_val), i32);

emit_flags_arith(result, v_flag, ac0);
```

`emit_flags_arith` handles AZ, AN, V, VS (sticky), AC0, V_COPY, AC0_COPY in one call.
This same pattern applies to any 32-bit add instruction: `dsp32alu_ADD32` and
`COMPI2opD_ADD` share identical overflow/carry formulas.


## pseudoDbg_Assert: ASTAT and AX register reads need special handling

`decode_pseudoDbg_Assert_{lo,hi,low,high}` use `allreg_offset(grp, regtest)` to load a
register value. Two special cases need to be handled before calling `allreg_offset`:

1. **ASTAT (grp=4, reg=6, fullreg=38)**: `allreg_offset` has no case for this.
   Must call `emit_astat_compose()` instead, just like `decode_REGMV` does.

2. **A0.X / A1.X (fullreg 32 / 34)**: stored as 8-bit values in `ax[0]`/`ax[1]`,
   but reads must sign-extend from 8 bits:
   ```cpp
   val = CreateAShr(CreateShl(val, 24), 24, "ax_sext");
   ```
   This matches `reg_read` in `refs/bfin-sim.c` which sign-extends AX values with
   `if (value & 0x80) value |= 0xFFFFFF00`.

Missing either guard results in DBGAL/DBGAH assertions silently reading wrong values
(ASTAT falls back to R0's offset; AX reads as unsigned 8-bit instead of signed 32-bit).


## SIGNBITS on 16-bit half: use sign-extension to i32, not shift-to-top trick

For `SIGNBITS Rs.L` and `SIGNBITS Rs.H`, the approach of shifting the 16-bit value
to the top of an i32 and applying ctlz is **wrong** for negative values because the
low 16 bits pollute the XOR'd normalize step.

**Correct approach**: sign-extend the 16-bit half to i32, then apply ctlz:

```
sext32    = SExt(Trunc(rs, i16), i32)   ; sign-extend Rs.L (or Rs.H after shift)
sign32    = AShr(sext32, 31)
norm32    = XOR(sext32, sign32)
clz       = ctlz(norm32, false)
result    = Sub(clz, 17)                ; 17 = 16 sign-extension bits + 1 sign bit
```

For 0 (positive): `sext=0`, `norm=0`, `ctlz(0)=32`, `32-17=15` ✓
For 0x8000 (most negative): `sext=0xFFFF8000`, norm=0x00007FFF, `ctlz=17`, `17-17=0` ✓
For 0xFFFF (all ones): `sext=0xFFFFFFFF`, norm=0, `ctlz=32`, `32-17=15` ✓

For 32-bit SIGNBITS, apply `min(ctlz-1, 30)` — ctlz(0)=32 gives raw result 31, but the
Blackfin spec caps redundant sign bits at 30 for a 32-bit operand.


## BXOR3 (sopcde=12, sop=1): A0 is NOT written back

The reference C for `Rd.L = CC = BXOR(A0, A1, CC)` is:

```c
SET_CCREG(CCREG ^ xor_reduce(acc0, acc1));
acc0 = (acc0 << 1) | CCREG;       // ← local variable only; NO SET_AREG call
SET_DREG(dst0, REG_H_L(DREG(dst0), CCREG));
```

The `acc0 = (acc0 << 1) | CCREG` line modifies a **local variable** — there is NO
`SET_AREG(0, acc0)` call. The A0 accumulator is left unchanged.

This is in contrast to BXORSHIFT3 (sop=0), which DOES call `SET_AREG(0, acc0)` to
write the shifted value back.

Implementing this incorrectly (writing back acc0) causes a cascading error in sequences
like `BXOR(A0,A1,CC); BXORSHIFT(A0,A1,CC)` because the BXORSHIFT uses the wrong A0.

**Debugging tip**: add `dbg A0;` pseudo-instructions between steps in a minimal assembly
test and run through `bfin-elf-run` to verify A0 does not change after BXOR3.


## emit_byteop_pack2 hi_slot: use (b1<<24)|(b0<<8), not shift of packed value

`emit_byteop_pack2(b0, b1, hi_slot=true)` packs two byte results into the high byte
slots of a 32-bit register (bytes 3 and 1). The reference formula is:

```c
STORE(DREG(dst0), (tmp1 << (16 + HL*8)) | (tmp0 << (HL*8)));
// HL=0: (b1 << 16) | b0           bytes 2,0
// HL=1: (b1 << 24) | (b0 << 8)   bytes 3,1
```

**Wrong approach** (shifts packed value left by 16):
```cpp
packed = (b1 << 16) | b0;
return hi_slot ? packed << 16 : packed;
// (b1 << 16) | b0  →  << 16 → b1 shifts to bit 32 and is truncated away
```

**Correct approach** for hi_slot=true:
```cpp
return B.CreateOr(B.CreateShl(b1, B.getInt32(24)),
                  B.CreateShl(b0, B.getInt32(8)));
```

Affected instructions when broken: BYTEOP2P_(RNDH/TH/RNDH_R/TH_R) and
BYTEOP3P_(HI/HI_R) — all callers of `emit_byteop_pack2(..., true)`.


## dsp32mac op=3 write: AZ and AN flags must be updated

When a dsp32mac instruction writes directly from the accumulator without multiply
(`w=1 && op==3`, e.g. `R0.L = A0 (IS)`, `R0.H = A1`), AZ and AN flags are updated.

Per `refs/bfin-sim.c` `decode_dsp32mac_0` lines 3938–3945:
```c
if ((w0 == 1 && op0 == 3) || (w1 == 1 && op1 == 3)) {
    SET_ASTATREG(az, zero);   // zero = any written result is 0
    SET_ASTATREG(an, n_0 | n_1);  // n_x = sign bit of extracted result
}
```

This only applies when `op==3` (no multiply, just reading the accumulator). When a
multiply is involved (`op=0/1/2`), AZ/AN are **not** updated by the mac instruction.

In `emit_dsp32mac`, compute after the V/VS update:
- For non-P mode: sign bit = bit 15 (result is 16-bit in low half)
- For P mode: sign bit = bit 31 (result is 32-bit full word)


## An optimization level must reach the code that runs

`JitEngine::optimize_module` was called only from `Core::run`'s IR-dump branch from
the initial commit on. A nonzero `opt_level` therefore changed the IR that `--dump`
printed and nothing that executed, and any timing of "the IR pipeline" measured the
backend alone. `Core::run` now optimizes every module between translation and
`addModule`, and the dump prints the module after that.

Turning the pipeline on also means poison is no longer harmless. The backend-only
path happened to shift by the count mod 32, so `shl i32 x, 32` and a negative
variable shift count "worked"; InstCombine folds both to anything. Pin the reference
behaviour explicitly (`& 31`, or the constant result) instead of relying on x86.

The pass pipeline needs the compiler's TargetMachine and every module the JIT's
DataLayout, both taken from the same `JITTargetMachineBuilder`/LLJIT: without them
the passes cost-model a generic target with no vector registers and default
integer widths. LoopIdiomRecognize can emit `memset`/`memcpy`/`memmove`, so those
are registered as absolute symbols with the rest.

## Profiling a JIT-heavy thread: never call backtrace() from a signal handler

Sampling a JIT-dispatching thread with SIGPROF: the handler must not call
`backtrace()`. On glibc/x86-64, `backtrace()` unwinds via libgcc's
`_Unwind_Backtrace`, which consults the dynamic loader's EH-frame registry
under a non-recursive lock, the *same* lock the JIT takes when registering
unwind info for a freshly emitted block. If the signal lands while the
thread holds that lock, the handler self-deadlocks the thread against
itself (observed reliably: thread parked in `futex_wait_queue` with SIGPROF
blocked in `SigBlk`, i.e. stuck inside its own handler).

Instead, walk the saved-RBP frame chain from the interrupted `ucontext_t`
(reads only the thread's own stack, no libc/loader calls), and symbolize
later, off the signal path. Better still, sample with `perf` and the jitdump.

Two sibling traps from the same campaign:

* `setitimer(ITIMER_PROF)` delivers to an arbitrary thread in the process;
  on a GUI application you end up sampling the GLFW/audio threads instead of
  the CPU thread. Use `timer_create(CLOCK_THREAD_CPUTIME_ID, SIGEV_THREAD_ID)`.
* Attributing JIT frames to guest PCs by "nearest preceding block address,
  capped at N bytes" silently misattributes samples beyond the cap. Take
  exact symbol sizes from the object file in `notifyObjectLoaded` (via
  `llvm::object::computeSymbolSizes`) and do exact range containment.

## jitdump listeners must be re-registered after invalidate()

`Core::invalidate()` replaces the JitEngine. A `JITEventListener` (e.g.
LLVM's `PerfJITEventListener` for `perf inject --jit`) registered on the old
engine silently stops recording everything compiled after the invalidation:
a boot that invalidates at the bootrom entry produced a jitdump covering
only the 215 pre-invalidation blocks out of ~21k. `attach_listeners()`
re-registers on every new engine.

Relatedly: killing the profiled process with default SIGTERM handling
truncates the jitdump stream (the listener flushes from its destructor).
Hosts that stop runs via signals should convert them into a clean shutdown.

## Where compiled code lies matters as much as what it is

LLJIT's default object layer gives every object its own SectionMemoryManager,
which maps each section separately, page-rounded. With one module per guest
block, every block started at offset 0 of a page of its own (op1emu: 21,338
blocks on 21,101 code pages): all block entries fell into the same L1I and L2
sets, and the blocks outside loops spent a quarter of their cycles in
instruction-cache data stalls. `CodeArena` packs them; that alone took only a
few points off those stalls, because each block's function still carried its
rare paths (slow guest accesses, did_jump exits, CEC calls) between its hot
path and the next block's -- 56% of the bytes of one of the hottest blocks.

What paid was telling LLVM which paths are rare. Unweighted, the per-packet
loop-bottom and interrupt checks laid their call paths inline with a taken
jump over them on every packet; with `!prof` weights (and `cold` on the rare
helpers) MachineBlockPlacement moves them to the function's tail. The machine
function splitter then moves them out of the function, into `.text.split.*`
sections the arena keeps in a region of their own. The splitter needs profile
counts and, by default, a profile summary: bcore gives every block function a
synthetic entry count after the IR pipeline and sets `mfs-psi-cutoff=0` with
an explicit `mfs-count-threshold`. Pick the rare weight so that a rare path's
count stays below anything an ordinary path can reach: guest conditional
branches are unweighted, so a path behind n of them counts entry/2^n (rare
1:2^20, entry 2^30, threshold 2^11). The splitter does nothing at
CodeGenOpt::None.

An entry count changes more than the splitter: with profile data,
MachineBlockPlacement trusts the static frequency estimate, which gives every
unweighted branch 1 in 2. The FIR filler's native loop goes back through six
such branches, so it looked as if it ran again once in 64 rather than 63 times
in 64, and its back edge went out of line (three taken jumps per iteration,
+13% cycles in the loop). Weight the branches whose way is known -- loop
bottoms, back edges, the chain exit -- before giving functions counts.

Two things that did not matter: transparent huge pages for the arena (same
cycles within noise; `icache_tag.stalls` are L1I tag misses, not iTLB walks),
and the arena's RWX permissions versus per-object `mprotect`.

One that cost compile time: inlining a hardware-loop count-down on every
taken-branch exit grew the IR of branch-heavy blocks enough to make the ISA
test `divq.s` 25% slower to compile; the rare path now calls the helper.

## What a chained block transition costs, and where it went

A block that runs the next one itself (a musttail call through the chain
table) used to pay, per transition: the six callee-saved pushes and pops of
the C convention, reloads of the pc and packet index its own path had just
stored, a computed table index even for a constant successor, a load of the
CEC's pending flag, and stores of insn_len, the packet index and did_jump
that the next block overwrote before anything read them -- about 45 machine
instructions, a third of the generated code's instructions outside loops in
the boot to op1emu's main display.

- Blocks are `ghccc`: the GHC convention saves no register for its caller,
  and `musttail` between two `ghccc` functions of the same type works in
  LLVM 15 (one `push`/`pop` stays, for stack alignment before helper calls).
  The host enters through one JIT-compiled C-ABI trampoline
  (`JitEngine::enter`), which saves the callee-saved registers once per run.
- Each return gets its own exit, with the values its path stored found by
  walking back through the block and its single predecessors. A constant
  successor reads its slot at a constant offset; a constant jump to the
  block's own start is a branch. The exit with a run-time PC must keep the
  native-loop branch too: a hardware loop's back edge goes through it (the
  first version without it cost 7% more instructions in the boot).
- No pending-interrupt test at exits: every instruction that may enable an
  interrupt is followed by a check, and the other ways to leave one pending
  (`cec_raise`, an IMASK write) set the chain budget to 0.
- The stores that end a return's path (pc, insn_len, packet index, did_jump)
  are made only where the block returns. Skipping the did_jump clear on the
  chain is right only where every path into that exit stored did_jump itself:
  `bfin_hwloop_step` sets it on a back edge with no store in the IR, and a
  shared exit that mixed both made the ISA test `c_mmr_loop` hang.

Together, host instructions in execution 62.99G -> 58.49G (-7.1%) with the
workload identical; cycles fell most in the blocks outside loops (-14%).
Whether the native loop may run is now decided at translation time from the
chain filter, so setting a filter drops the blocks already compiled.

## Loops as compilation units (loop blocks, proven copies, speculative sums)

A block that runs itself again (a hardware loop's top, a jump to its start)
is lifted a second time as a loop block: the visitor's cpu pointer is an
alloca shadow that SROA turns into SSA values, so the loop's registers stay
in host registers across its back edge. Every helper call is bracketed by
placeholder calls the translator expands afterwards into copies of the
fields that are dirty there (a forward dataflow over the lifted function),
and every exit copies the dirty fields out. Lessons:

- Keep only what the loop touches in the shadow. With every register in it,
  the loop carried ~30 PHIs across the back edge and spilled; copying only
  dirty fields at syncs, and shadowing only fields the loop's packets reach,
  cut that to the dozen the loop really changes.
- The memory callbacks never change the registers unless they raise an
  exception, which sets did_jump and leaves the block: sync in only on that
  path. Syncing in unconditionally after every slow access made every value
  a merge of the fast and slow paths again.
- The accumulator needs a whole i64 copy in the shadow (past CpuState): split
  into ax/aw fields, each iteration rebuilt it from two PHIs with shl/ashr/or
  and LLVM never saw one value. Keep both fields in the shadow together, or
  the copy goes stale when only one is written.
- Stores of a parallel packet are deferred to its end; so must the whole
  accumulator's be, or its latch value is the merge of the field stores and
  not the MAC's result.
- The chain budget: compare with the budget read once at entry, then test
  the reloaded one against 0. The same test (the budget only ever drops to
  0), but SCEV counts the loop by the first half.
- `(s + 2^31) <u 2^32 ? sext(trunc s) : clamp` is the W32 window test in a
  form LLVM knows yields an i32; the reference form (shift by 31, compare
  with 0 and 0x1ffffffff, mask, re-extend) costs ~10 more instructions per
  MAC in every block, loop or not.
- Write the doubled W32 product of two signed halves in i32 (`mul nsw` of
  two sext16): computed in i64 and truncated, the vectorizer kept <8 x i64>
  multiplies.

Guest memory in loop blocks goes through two readnone calls
(`bcore.guest.ptr`, `bcore.guest.aligned`). `GuestLoopVersioning` proves
each affine access's range once at the loop's entry (one mapped page,
aligned) and copies the loop with direct accesses; whatever is left is
lowered to the page table afterwards. A speculative native load before the
guard exists would be UB (and poison could delete the guard), hence the
abstract calls. The proven pointer must be base + an i64 AddRec expanded in
the loop *before* it is cloned: a 32-bit `addr - page_va` zero-extended is
not affine to SCEV (no nuw proof), and LoopVectorize then saw no consecutive
access. The expander may hoist an invariant offset out of the loop, so the
clone map can miss it.

`SatReductionSpeculation` gives a W32 MAC loop a copy without clamps, run
for m-1 iterations with each term rotated one iteration earlier (the
software-pipelined loads feed the next iteration's MAC: the copy uses the
values iteration j leaves for j+1, the term of iteration 0 peeled before
it), so that every PHI but the sums is an induction. Its i32 sum is exact
where |a0| + m·max|x| ≤ INT32_MAX; the loop then resumes at iteration m,
every other PHI rebuilt there (inductions by SCEV, the rest by cloning the
loop's slice one iteration back). LoopVectorize refuses a loop with any
live-out that is not a reduction or induction, which is why the rebuild
happens outside the copy. LLVM 15 has no saturating RecurKind.
- An IRBuilder at the end of a block has no instruction to give
  SCEVExpander as an insertion point: put a temporary terminator there.
- `cloneLoopWithPreheader` does not add the exit PHIs' entries: add them at
  once, and call removePredecessor where the copy's exits go, so the IR is
  valid on every path out of the transform.
- The copy's stop test (the chain budget's atomic load) must go wherever it
  is, the latch included: no pass deletes an ordered load, and LoopVectorize
  refuses a loop with one ("read with atomic ordering"). With chained blocks
  counted the latch is the hwloop's back edge and the test sits before it;
  uncounted (Core::set_count_chained(false), op1emu's release build) the test
  is the latch's own branch, which the copy's counting branch replaced and
  left the load behind. The FIR loop then went unvectorized in every release
  build from the start, while the profiling builds that measured it counted
  chained blocks and were fine. LLVM said so only as "loop not vectorized:
  the optimizer was unable to perform the requested transformation" on
  stderr; the reason is a LoopVectorize analysis remark, which a
  DiagnosticHandler on the context shows.

Measured (boot to op1emu's main frame, workload and digests identical with
and without loop blocks, and dry): host instructions in execution 57.8G ->
19.8G; the firmware's FIR loop (0x01a5e27e) 1.73 s -> 0.12 s.

## Loops that wait (SteadyLoopSkip)

Firmware waits in loops that poll -- a device status register, a flag in
RAM, a hardware loop that counts down a timeout -- and within a run nothing
they read can change: the host's devices change between runs, no other code
runs. An iteration is then the previous one plus its counters. The pass
classifies a loop block's header PHIs by SCEV: affine with a nonzero step
(the counters: chain_packets, packets, chain_blocks, LC0, a register the
loop increments) or not (they must come back to their values, compared at
the latch). Everything computed from a counter may only be stored to
CpuState, decide an exit SCEV counts, flow back into a header PHI or leave
through an LCSSA PHI. With that, a steady iteration means every later one up
to the first counted exit is the same, and the latch selects each counter's
value at that iteration (expanded in the preheader). Iteration N then runs
as always and exits where it would have.
- The run's budget was no exit SCEV could count for a loop of more than one
  packet: `chain_packets + packets` had no nuw and the budget any i32. Range
  metadata on the budget loads and a nuw add made them countable, which also
  gave GuestLoopVersioning ~25% more proven loops.
- Device reads go through callbacks: the host says which it cannot promise
  would repeat (a FIFO pop, a time-derived count) by counting them in
  CpuState::unstable_reads; the latch compares it with its value at the top
  of the iteration. A host that does not count must not turn the pass on.
- A sync before a callback copies the dirty shadow fields only: after a skip
  a counter field is clean where the real state still holds its value from
  before the skip. With the pass on, every copy out writes every field the
  block stores anywhere.
- Values also flow between iterations through CpuState itself: a field the
  loop stores and loads back. Allowed only where the stored value is not
  derived from a counter (did_jump, copied out before a callback that may
  set it, then read).
- A loop block whose accesses go inline (rawmem) has no bcore.guest calls,
  so the loop-block pipeline (SROA first) never ran: the pass then found no
  header PHIs at all. It runs its own SROA in that case.

Measured on op1emu's synth phase (main frame to 10 s of guest time): device
reads 52.2M -> 12.3M, every other workload counter and digest identical.

## Circular buffers in loop blocks

A circular DAG register (L != 0) stepped by bfin's dagadd/dagsub is a chain
of 64-bit carry tests and selects, and its address is no AddRec, so neither
SCEV nor GuestLoopVersioning could do anything with the loop around it
(op1emu's dsp_fir_convolution: a two-packet dual-MAC hardware loop, I1 a
ring of samples). Under an entry guard -- L != 0, L <= B, B + 2L <= 2^32,
I - B < L, every step at most L in size, B, L and the modifiers unwritten,
I written by DAG updates only -- I stays in [B, B + L): I + d lies in
[B - L, B + 2L), neither end wraps, every carry test comes out the same
way, and the update is t = I + d; t >= B + L ? t - L : t; then < B ? + L.
(L <= B matters: with B < L a negative step wraps below 0, where dagadd
adds L and the short form does not.) The accesses through I go through
bcore.guest.ptr.in(table, addr, B, L), which GuestLoopVersioning proves
from the window at entry like an affine access (one mapped page holding
[B, B + L) plus the width) and lowers to page + (addr - page_va).
loop_block_test randomizes I1's buffer and step, larger than the buffer
included, against plain blocks.

## Compile time: tiers, and what did not pay

op1emu's boot to its main display spent 229 s compiling 21,411 blocks and
1.6 s running them. Blocks that ran fewer than 1000 times took 83% of the
compile time and executed 0.9% of the packets; 8,334 ran once. Even a block
of 45 IR instructions cost 1.8 ms.
- ORC's SimpleCompiler builds a new legacy PassManager and calls
  addPassesToEmitMC for every module: 6.7 s of the 229. Compiler (compiler.h)
  builds it once over a persistent raw_svector_ostream and runs it module
  after module, which MachineModuleInfoWrapperPass supports: its
  doInitialization/doFinalization start and reset the MMI and MCContext.
  The objects are byte-identical; LLJIT gets them as object files.
- CodeGenOpt::Less produces byte-identical code to Default on x86-64 in
  LLVM 15: a level that changes nothing.
- Tier 0 is the plain block, no IR pipeline, CodeGenOpt::None (FastISel,
  RegAllocFast) and no function splitter: about a quarter of the full cost.
  FastISel selects bottom-up and gives the rest of a basic block to
  SelectionDAG at the first instruction it cannot select; X86FastISel
  selects no return in the GHC convention, so every basic block ending in
  `ret` was SelectionDAG's. One return per tier-0 block: -2.0%. (Making
  block functions nounwind saved 0.5% more, and turned a host callback's
  exception, which had unwound through generated code to the host, into
  std::terminate: not done.)
- A promotion must run the code its tier-0 block ran and see the guest state
  that block was compiled with: the lift reads code through the host, whose
  reads may have side effects and whose memory may have changed since.
  RecordingMemory logs every halfword the tier-0 lift reads; the promotion
  reads through a ReplayMemory that answers from that log only, with a copy
  of the CpuState the tier-0 lift took its hints from.
- Every path that lifts reads Memory::page_table() and must leave the table
  in CpuState for the code it inlines through: the worker's promotions
  once skipped that. And every compilation of a PC, tiered or not, decides
  which queued promotion may still replace it.
- Counters live after the chain table (TierTable, reached through
  CpuState::chain_table), indexed by tier-0 compilation, not by chain slot:
  a slot is shared by colliding PCs and absent for PCs the filter rejects.
- On a worker thread the lift stays on the dispatching thread (the host's
  reads again) and so do loading and publishing (the JIT and the chain
  table are single-threaded). With guest time in packets, where a
  promotion's code replaces the tier-0 code must not depend on the host's
  speed: SteadyLoopSkip, for one, skips device reads only in tier-1 code.
  publish_after puts that point a fixed number of packets after the request
  and makes the dispatching thread wait for the worker there.
- Batching promotions into one module (one context, Linker, one object) was
  built and measured: the worker's CPU went up 6%, waits too. Once the
  backend's pipeline is built once, what a small block costs is per
  function, and filling a batch delays its publication.
- The threshold trades JIT time for execution: on the boot, from 300 to
  100000, all JIT CPU 79 -> 32 s and execution 2.1 -> 2.9 s. op1emu uses
  30000.
- With tier 0 made of stencils, the worker's promotions became most of the
  JIT's CPU (8.4 of 15.1 s on the boot), and most of them did not pay: 419
  of 470 (8.0 s) were blocks just past 30,000 starts that ran little after
  it (0x0197d522: 32,512 starts in all, 2,909 IR instructions, 110 ms).
  What a promotion costs grows with its block; what it saves with the
  packets the block runs after it. Raising the threshold for every block
  costs execution fast (100,000: the worker 3.4 s, execution host
  instructions +12%; no tier 1 at all: execution 2.3 -> 5.8 s). Scaling it
  by the block's size (tier_threshold: a block of n packets, n > size_ref,
  waits n / size_ref times as long) is a better trade: size_ref 16 took
  the worker from 8.1 to 5.0 s for 7% more execution (+0.16 s), 8 to
  3.6 s for 15%. Counting packets run instead of starts would be better
  still on paper (a census estimate: 44% of the compile for 94% of the
  benefit), but needs every exit to add to the count.
- The light tier-1 pipeline (Compiler::kLightPipeline: no module or loop
  passes) for promotions other than loop blocks took the worker a further
  7% down (5.0 -> 4.7 s) with execution unchanged. Loop blocks keep O1:
  their loops are where the pipeline earns its time.
- A BBTranslateResult nobody took apart (a promotion dropped because its
  lift read code its tier-0 block had not) freed its module after its
  context, a use after free: members are destroyed last to first, so the
  context comes first now.

## Copy-and-patch tier 0

- What a tier-0 block costs is LLVM's, about 1.1 ms for a median block of 8
  packets, linear in its IR: FastISel and the MC layer, whatever the IR
  pipeline does. Blocks copied together from stencils compiled at build
  time take 4 us (median; 9,000 host instructions, mostly the Tier0Record's
  copy of CpuState and the relocations).
- The stencils come from the lifter itself (BBTranslator::translate_stencil
  in stencil mode), so they cannot drift from it: every 16-bit encoding
  separately (49,808 of 49,920; the rest load a constant pool), and the
  32-bit patterns with their immediates as holes. Holes for the register
  fields would have needed the lifter rewritten, since its C++ decides on
  register numbers (REGMV, ASTAT, push/pop of RETI and USP, the CC forms).
- Check what you cannot see: a lift at two PCs must be the same IR, a
  32-bit pattern's lifts with other hole values the same IR with the values
  its formula gives, and every relocation one the translator fills in.
  Comparing IR text, number the attribute groups out: a module numbers them
  as functions come, and a check lifts one function after another.
- The packet state stays lazy across stencils: each stores its own PC,
  length and index at its top for the sink to move to their readers, and
  the jump to the next packet reads nothing. The hot path of a plain ALU
  instruction then stores no PC at all.
- Stencil code has no unwind information; the helpers that may reach the
  host's code are wrapped, catch, set did_jump and a budget of 0 (the block
  leaves at once), and Core::run throws again. Without a wrapper the
  exception is std::terminate (tier_test checks).
- Coverage decides the gain: 16-bit-only blocks were 5% of the boot's tier-0
  time; with CALL, LDIMM, LSETUP, LDSTidxI and LINK 48.5%, and the CPU
  thread's compile time fell 47%. Blocks with a parallel packet are nearly
  all the rest.
- What the stencils cost at run time: each exit goes through the arena's
  shared chain exit (3% of a boot's execution samples) and each packet
  compares the loop bottoms, calling bfin_hwloop_step at one (2.6%); the
  execution's host instructions still rose only 0.2%.
- The rest, built at run time: on the boot to the main display the LLVM
  tier 0 still had 7,831 blocks, 15.2 s of the CPU thread's 15.5 s of
  compilation, for 8,562 distinct packets without stencils (6,919
  parallel). Lifted and checked as the build lifts its own (no field holes:
  one stencil per encoding) and compiled at -O0 in one module per block,
  they cost 0.5 ms each; 24 LLVM blocks are left, and the CPU thread's
  compile time fell 55% (15.0 to 6.7 s), the wall time 20.0 to 12.5 s.
  Execution rose 7% (0.1 s): -O0 stencils, and every packet's state through
  CpuState where LLVM's block kept it in registers. At -O2 (the build's
  level) compilation was 3.5 times as long for 8% less execution.
- Run time and build must lift the same: one StencilMemory and one check
  procedure (cp_build), and at -O2 the run time builds 16-bit stencils
  byte for byte as the build did (cp_runtime_test). Its lift must not read
  past the packet: a stencil would hold what followed it.
- A run-time stencil holds for any PC (the same IR lifted at two PCs) and
  is kept for the process, past Core::invalidate; so is a refusal, so that
  a packet that cannot be a stencil costs one try.
- That check printed both lifts and compared the text: 37% of the CPU
  thread's compile instructions, more than anything but the backend
  (formatted_raw_ostream counts columns as it prints). Lifting the second
  into the same module and comparing with llvm::FunctionComparator
  (same_ir) keeps the check for a fraction: the CPU thread's compile time
  6.5 -> 5.5 s (instructions 57.7 -> 40.3 G), the code the same.
- What the remaining run-time stencils cost is LLVM's backend at -O0,
  half of it SelectionDAG. X86FastISel selects no address of an
  absolute_symbol global (every hole is one) and no GHC return or
  arguments; without absolute_symbol it loads each hole with a 10-byte
  movabs (R_X86_64_64), worse code for 6% less compile. Building only the
  stencils a run reaches (a block ending before a packet without one) saves
  little: 8,306 of 8,602 are reached on the boot.
- Found in review: code placed in an arena outside an object must live as
  long as the engine, since the cache and the chain table point into it
  (JitEngine keeps every arena it gave out); a tier that reaches RAM
  through the page table must ask the Memory whether it shows one now, not
  use the table noted earlier (a host hides it to see every access); and
  the core must build without the tier, which CI now does on x86-64 too.
