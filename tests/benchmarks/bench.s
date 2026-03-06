// Blackfin micro-benchmark: integer ALU, DSP MAC, load/store, dot-product,
//                           subroutine calls, branch-intensive loops
//
// Six kernels:
//   K1 - pure integer ALU          100 M single-insn iterations
//   K2 - 16x16 IS MAC              100 M single-insn iterations
//   K3 - sequential 32-bit loads   11.2 M outer x 8 inner = 89.6 M loads
//   K4 - load-load-MAC dot product   9 M outer x 8 inner = 72 M load pairs + 72 M MACs
//   K5 - subroutine call overhead   25 M CALL (Pn) + RTS round-trips
//   K6 - 3-level nested branch stress   2 M × 4 × 5 = 40 M branches, count-up+down mix
//
// Results are verified with CHECKREG; the binary exits 0 on success.
//
// Usage:
//   time ./build/emu build/test_linked/bench.s.elf
//
// Tuning: scale the imm32 iteration counts to hit the desired wall time.
//
# mach: bfin

.include "testutils.inc"
	start

/* -------------------------------------------------------
 * Kernel 1: Integer ALU
 *
 * Single-instruction hardware loop — stresses pure integer
 * add throughput and BB translation cache.
 *
 * R0 = R0 + R1 (R1 = 3), 100 000 000 iterations
 * Expected: R0 = 3 * 100 000 000 = 300 000 000 = 0x11E1A300
 * ------------------------------------------------------- */
	R0 = 0;
	R1 = 3;
	imm32 R2, 100000000;
	P0 = R2;

	LSETUP (k1, k1) LC0 = P0;
k1:	R0 = R0 + R1;

	CHECKREG R0, 0x11E1A300;

/* -------------------------------------------------------
 * Kernel 2: DSP MAC
 *
 * Single-instruction MAC loop — stresses 16x16 integer
 * signed multiply-accumulate and 40-bit accumulator.
 *
 * A0 += R2.L * R3.L (IS), R2.L = 3, R3.L = 4, 100 000 000 iterations
 * Expected: A0.w (lower 32 bits) = 12 * 100 000 000 mod 2^32 = 0x47868C00
 * ------------------------------------------------------- */
	A0 = 0;
	R2.L = 3;
	R3.L = 4;
	imm32 R4, 100000000;
	P0 = R4;

	LSETUP (k2, k2) LC0 = P0;
k2:	A0 += R2.L * R3.L (IS);

	R0 = A0.w;
	CHECKREG R0, 0x47868C00;

/* -------------------------------------------------------
 * Kernel 3: Sequential Load + Accumulate
 *
 * Manual outer loop drives an 8-element inner hardware
 * loop.  Each inner iteration loads a 32-bit word via
 * mem_read32 and accumulates it.  Tests memory helper
 * throughput — the dominant cost in the JIT.
 *
 * 11 200 000 outer x 8 inner = 89 600 000 loads total
 * sum([1..8]) = 36 per outer pass
 * Expected: R0 = 36 * 11 200 000 = 403 200 000 = 0x18085800
 * ------------------------------------------------------- */
	imm32 R6, 11200000;
	P1 = 8 (X);
	R0 = 0;

k3_outer:
	loadsym P0, _bench_data;
	LSETUP (k3_top, k3_bot) LC0 = P1;
k3_top:	R1 = [P0++];
k3_bot:	R0 = R0 + R1;
	R6 += -1;
	CC = R6 == 0;
	IF !CC JUMP k3_outer;

	CHECKREG R0, 0x18085800;

/* -------------------------------------------------------
 * Kernel 4: Dot-Product (Load-Load-MAC)
 *
 * Manual outer loop drives a 3-instruction inner hardware
 * loop: two sequential 32-bit loads followed by a 16x16
 * IS MAC.  Interleaves memory and DSP unit pressure.
 *
 * 9 000 000 outer x 8 inner = 72 000 000 load pairs + 72 000 000 MACs
 * bench_data[i].L * coef_data[i].L = i * 2  (coef all 2)
 * sum per outer pass = 2*(1+2+3+4+5+6+7+8) = 72
 * Expected: A0.w (lower 32 bits) = 72 * 9 000 000 mod 2^32 = 0x269FB200
 * ------------------------------------------------------- */
	A0 = 0;
	imm32 R7, 9000000;
	P2 = 8 (X);

k4_outer:
	loadsym P0, _bench_data;
	loadsym P1, _coef_data;
	LSETUP (k4_top, k4_bot) LC0 = P2;
k4_top:	R1 = [P0++];
	R2 = [P1++];
k4_bot:	A0 += R1.L * R2.L (IS);
	R7 += -1;
	CC = R7 == 0;
	IF !CC JUMP k4_outer;

	R0 = A0.w;
	CHECKREG R0, 0x269FB200;

/* -------------------------------------------------------
 * Kernel 5: Subroutine-call overhead (CALL + RTS)
 *
 * Manual counter loop calls a minimal stub 25 000 000 times.
 * Each CALL saves RETS; the stub increments R5 then RTS.
 * Stresses JIT re-entry across two basic blocks per
 * iteration and the RETS save/restore path.
 * (CALL inside LSETUP is avoided — hardware-loop CALL
 *  interaction is undefined on ADSP-BF5xx.)
 *
 * Expected: R5 = 25 000 000 = 0x17D7840
 * ------------------------------------------------------- */
	R5 = 0;
	imm32 R4, 25000000;
	loadsym P5, _nop_sub;

k5:	CALL (P5);
	R4 += -1;
	CC = R4 == 0;
	IF !CC JUMP k5;

	CHECKREG R5, 0x17D7840;

/* -------------------------------------------------------
 * Kernel 6: 3-level nested branch stress
 *
 * Three nested IF !CC JUMP loops using two different
 * comparison styles:
 *   outer — count-down:  CC = R6 == 0  (1 000 000 iters)
 *   mid   — count-up:    CC = R4 == 4  (4 iters per outer)
 *   inner — count-down:  CC = R3 == 0  (5 iters per mid)
 *
 * This creates five distinct basic blocks and exercises
 * both "decrement to zero" and "increment to limit"
 * branch patterns in the JIT's BB cache.
 *
 * 2 000 000 outer x 4 mid x 5 inner = 40 000 000 R0 += 7
 * Expected: R0 = 7 * 40 000 000 = 280 000 000 = 0x10B07600
 * ------------------------------------------------------- */
	R0 = 0;
	R1 = 7;
	R2 = 4;                 /* mid-level trip count (compare target) */
	imm32 R6, 2000000;

k6_outer:
	R4 = 0;
k6_mid:
	R3 = 5;
k6_inner:
	R0 = R0 + R1;
	R3 += -1;
	CC = R3 == 0;
	IF !CC JUMP k6_inner;
	R4 += 1;
	CC = R4 == R2;
	IF !CC JUMP k6_mid;
	R6 += -1;
	CC = R6 == 0;
	IF !CC JUMP k6_outer;

	CHECKREG R0, 0x10B07600;

	pass

	.text
	.align 2
_nop_sub:
	R5 += 1;
	RTS;

	.data
	.align 4
_bench_data:
	.long 1
	.long 2
	.long 3
	.long 4
	.long 5
	.long 6
	.long 7
	.long 8

	.align 4
_coef_data:
	.long 2
	.long 2
	.long 2
	.long 2
	.long 2
	.long 2
	.long 2
	.long 2
