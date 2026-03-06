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

