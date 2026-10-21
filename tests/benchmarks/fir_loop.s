// Blackfin micro-benchmark: the firmware's audio FIR filler loop shape.
//
// A single-packet hardware loop
//   A0 += R1.L * R0.H (W32) || R1.L = W[I2++] || R0 = [I0++]
// software-pipelined (the loads feed the next iteration's MAC), 63 inner
// iterations nested in a second hardware loop, as in the op1 firmware's
// thread_audio_fir (0x01a5e27e). The outer body restarts the pointers, adds
// the result to a checksum and repeats 64 times per pass.
//
// The coefficients and samples are small, so no step saturates; a second
// kernel uses values that do (W32 clamps and sets AV0/AV0S) to keep the
// saturating path exact.
//
// Usage:
//   time ./build/emu -O 1 --page-table --chain build/test_linked/fir_loop.s.elf
//
# mach: bfin

.include "testutils.inc"
	start

	imm32 R7, 20000;            /* passes */
	R6 = 0;                     /* checksum */
	P0 = 63 (X);                /* inner count, as the firmware's P0 */
	P1 = 64 (X);                /* outer count */

fir_pass:
	loadsym P4, _fir_out;
	LSETUP (fir_otop, fir_obot) LC1 = P1;
fir_otop:
	loadsym I0, _fir_samples;
	loadsym I2, _fir_coefs;
	A0 = 0;
	R0 = [I0++];
	R1.L = W[I2++];
	LSETUP (fir_in, fir_in) LC0 = P0;
fir_in:	A0 += R1.L * R0.H (W32) || R1.L = W[I2++] || R0 = [I0++];
	R2 = A0.w;
	R6 = R6 + R2;
fir_obot:
	[P4++] = R2;
	R7 += -1;
	CC = R7 == 0;
	IF !CC JUMP fir_pass;

	CHECKREG R6, 0x39ADA000;

/* Saturating: every product is 0x7fff * 0x7fff << 1, so A0 clamps at
 * INT32_MAX after two steps and AV0S stays set. */
	imm32 R7, 2000;
	R6 = 0;
sat_pass:
	LSETUP (sat_otop, sat_obot) LC1 = P1;
sat_otop:
	loadsym I0, _sat_samples;
	loadsym I2, _sat_coefs;
	A0 = 0;
	R0 = [I0++];
	R1.L = W[I2++];
	LSETUP (sat_in, sat_in) LC0 = P0;
sat_in:	A0 += R1.L * R0.H (W32) || R1.L = W[I2++] || R0 = [I0++];
	R2 = A0.w;
sat_obot:
	R6 = R6 + R2;
	R7 += -1;
	CC = R7 == 0;
	IF !CC JUMP sat_pass;

	CHECKREG R6, 0xFFFE0C00;
	R0 = ASTAT;
	CC = BITTST (R0, 17);       /* AV0S */
	IF !CC JUMP fail_lbl;

	pass
fail_lbl:
	fail

	.data
	.align 4
_fir_samples:
	.rept 64
	.long 0x00030000
	.endr
_fir_coefs:
	.rept 64
	.short 0x0002
	.endr
	.align 4
_sat_samples:
	.rept 64
	.long 0x7fff0000
	.endr
_sat_coefs:
	.rept 64
	.short 0x7fff
	.endr
	.align 4
_fir_out:
	.space 256
