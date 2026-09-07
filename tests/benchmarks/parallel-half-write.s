// Regression for the halfword pipeline used by b2BroadPhase::DestroyProxy.
# mach: bfin
.include "testutils.inc"
    start
    loadsym P2, values;
    P5 = 2;
    imm32 R0, 0xabcd0005;
    R6 = -1;
    R0.H = R0.L + R6.L (NS) || R0.L = W[P2++P5];
    CHECKREG R0, 0x00041234;

    // The ALU still reads the pre-issue high half while the load replaces it.
    imm32 R0, 0x00050000;
    R0.L = R0.H + R6.L (NS) || R0.H = W[P2++P5];
    CHECKREG R0, 0x56780004;

    // The other memory slot must see R0 before either half is written.
    loadsym P2, values;
    loadsym I0, saved;
    imm32 R0, 0xabcd0005;
    R0.H = R0.L + R6.L (NS) || R0.L = W[P2++P5] || [I0++] = R0;
    CHECKREG R0, 0x00041234;
    loadsym P0, saved;
    R1 = [P0];
    CHECKREG R1, 0xabcd0005;

    // The firmware uses this pair in a hardware loop to update a packed
    // array of counts. Exercise both the ordinary and internal Loop paths.
    loadsym P2, values;
    R0 = 5;
    P1 = 2;
    LSETUP (half_loop_top, half_loop_bottom) LC0 = P1;
half_loop_top:
    R0.H = R0.L + R6.L (NS) || R0.L = W[P2++P5];
half_loop_bottom:
    NOP;
    CHECKREG R0, 0x12335678;

    // slot0 dsp32shiftimm writes R0.H; slot1 load writes R0.L.
    // store_dreg_lo must merge against the shift's pending shadow write.
    loadsym P2, values;
    imm32 R0, 0xabcd0005;
    R0.H = R0.H << 1 || R0.L = W[P2++P5];
    CHECKREG R0, 0x579a1234;

    // Reverse direction: shift writes R0.L, load writes R0.H.
    loadsym P2, values;
    imm32 R0, 0xabcd0005;
    R0.L = R0.L << 1 || R0.H = W[P2++P5];
    CHECKREG R0, 0x1234000a;
    pass
.data
.align 4
values:
    .short 0x1234, 0x5678;
.align 4
saved:
    .long 0;
