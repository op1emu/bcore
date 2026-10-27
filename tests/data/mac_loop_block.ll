; A loop block's module as Compiler::optimize is given it, for
; tests/vectorize_count_test.cpp: a W32 MAC loop of loop_block_test's mac
; programs (one MAC with two loads in a hardware loop), lifted by bcore and
; reduced with llvm-reduce-15 to what keeps the test's facts: SatReductionSpeculation
; speculates its one loop; compiled for Zen 4 (to LLVM 15 znver3 with AVX-512,
; to LLVM 19 znver4) the vectorizer makes a vector loop and a vectorized
; epilogue, for x86-64-v3 one VF 8 loop.
; ModuleID = 'reduced.ll'
source_filename = "bb_module"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

define swifttailcc void @bb_0x00000104(ptr swiftself %cpu, ptr swiftasync %mem) #0 {
entry:
  %0 = getelementptr i8, ptr %cpu, i64 336
  %chain_table = load ptr, ptr %0, align 8
  %1 = getelementptr i8, ptr %cpu, i64 344
  %chain_budget = load atomic i32, ptr %1 monotonic, align 4
  %2 = getelementptr i8, ptr %cpu, i32 312
  %page_table = load ptr, ptr %2, align 8
  %shadow = alloca [464 x i8], align 8
  %3 = getelementptr i8, ptr %shadow, i64 0
  %4 = getelementptr i8, ptr %cpu, i64 0
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %3, ptr align 1 %4, i64 8, i1 false)
  %5 = getelementptr i8, ptr %shadow, i64 64
  %6 = getelementptr i8, ptr %cpu, i64 64
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %5, ptr align 1 %6, i64 4, i1 false)
  %7 = getelementptr i8, ptr %shadow, i64 72
  %8 = getelementptr i8, ptr %cpu, i64 72
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %7, ptr align 1 %8, i64 4, i1 false)
  %9 = getelementptr i8, ptr %shadow, i64 80
  %10 = getelementptr i8, ptr %cpu, i64 80
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %9, ptr align 1 %10, i64 4, i1 false)
  %11 = getelementptr i8, ptr %shadow, i64 88
  %12 = getelementptr i8, ptr %cpu, i64 88
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %11, ptr align 1 %12, i64 4, i1 false)
  %13 = getelementptr i8, ptr %shadow, i64 112
  %14 = getelementptr i8, ptr %cpu, i64 112
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %13, ptr align 1 %14, i64 4, i1 false)
  %15 = getelementptr i8, ptr %shadow, i64 120
  %16 = getelementptr i8, ptr %cpu, i64 120
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %15, ptr align 1 %16, i64 4, i1 false)
  %17 = getelementptr i8, ptr %shadow, i64 152
  %18 = getelementptr i8, ptr %cpu, i64 152
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %17, ptr align 1 %18, i64 4, i1 false)
  %19 = getelementptr i8, ptr %shadow, i64 188
  %20 = getelementptr i8, ptr %cpu, i64 188
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %19, ptr align 1 %20, i64 4, i1 false)
  %21 = getelementptr i8, ptr %shadow, i64 260
  %22 = getelementptr i8, ptr %cpu, i64 260
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %21, ptr align 1 %22, i64 8, i1 false)
  %23 = getelementptr i8, ptr %shadow, i64 292
  %24 = getelementptr i8, ptr %cpu, i64 292
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %23, ptr align 1 %24, i64 4, i1 false)
  %25 = getelementptr i8, ptr %shadow, i64 308
  %26 = getelementptr i8, ptr %cpu, i64 308
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %25, ptr align 1 %26, i64 4, i1 false)
  %27 = getelementptr i8, ptr %shadow, i64 348
  %28 = getelementptr i8, ptr %cpu, i64 348
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %27, ptr align 1 %28, i64 4, i1 false)
  %29 = getelementptr i8, ptr %shadow, i64 296
  store i8 0, ptr %29, align 1
  %30 = getelementptr i8, ptr %cpu, i64 64
  %31 = load i32, ptr %30, align 4
  %32 = getelementptr i8, ptr %cpu, i64 72
  %33 = load i32, ptr %32, align 4
  %34 = zext i32 %33 to i64
  %35 = trunc i32 %31 to i8
  %36 = sext i8 %35 to i64
  %37 = shl i64 %36, 32
  %38 = or i64 %37, %34
  %39 = getelementptr i8, ptr %shadow, i64 448
  store i64 %38, ptr %39, align 8
  br label %insn_0x00000104_entry

insn_0x00000104_entry:                            ; preds = %chain_go, %entry
  %40 = getelementptr i8, ptr %shadow, i32 292
  store i32 8, ptr %40, align 4
  %41 = getelementptr i8, ptr %shadow, i32 308
  store i32 1, ptr %41, align 4
  br label %insn_0x00000104

insn_0x00000104:                                  ; preds = %insn_0x00000104_entry
  %42 = getelementptr i8, ptr %shadow, i32 64
  %ax = load i32, ptr %42, align 4
  %43 = getelementptr i8, ptr %shadow, i32 72
  %aw = load i32, ptr %43, align 4
  %acc_slot = getelementptr i8, ptr %shadow, i64 448
  %acc = load i64, ptr %acc_slot, align 8
  %44 = getelementptr i8, ptr %shadow, i32 4
  %s0_32 = load i32, ptr %44, align 4
  %45 = getelementptr i8, ptr %shadow, i32 0
  %s1_32 = load i32, ptr %45, align 4
  %s1_hi = lshr i32 %s1_32, 16
  %s0_16 = and i32 %s0_32, 65535
  %s1_16 = and i32 %s1_hi, 65535
  %46 = trunc i32 %s0_16 to i16
  %s0_se = sext i16 %46 to i64
  %47 = trunc i32 %s1_16 to i16
  %s1_se = sext i16 %47 to i64
  %prod = mul i64 %s0_se, %s1_se
  %48 = lshr i64 %acc, 39
  %sgn40 = trunc i64 %48 to i1
  %49 = trunc i32 %s1_16 to i16
  %50 = sext i16 %49 to i32
  %51 = trunc i32 %s0_16 to i16
  %52 = sext i16 %51 to i32
  %prod32 = mul nsw i32 %52, %50
  %is_sat = icmp eq i32 %prod32, 1073741824
  %53 = shl i32 %prod32, 1
  %frac32 = select i1 %is_sat, i32 2147483647, i32 %53
  %prod_frac = sext i32 %frac32 to i64
  %acc_add = add i64 %acc, %prod_frac
  %54 = add i64 %acc_add, 2147483648
  %w32_fits = icmp ult i64 %54, 4294967296
  %w32_clamp = select i1 %sgn40, i64 -2147483648, i64 2147483647
  %55 = trunc i64 %acc_add to i32
  %56 = sext i32 %55 to i64
  %w32_sext = select i1 %w32_fits, i64 %56, i64 %w32_clamp
  %57 = xor i1 %w32_fits, true
  %58 = or i1 %57, %is_sat
  %acc_sat_w32 = zext i1 %58 to i32
  %59 = getelementptr i8, ptr %shadow, i32 264
  %avs_cur = load i32, ptr %59, align 4
  %60 = or i32 %avs_cur, %acc_sat_w32
  %61 = lshr i64 %w32_sext, 32
  %ax_new = trunc i64 %61 to i32
  %ax_8 = and i32 %ax_new, 255
  %aw_new = trunc i64 %w32_sext to i32
  %62 = shl i64 %w32_sext, 24
  %acc40 = ashr i64 %62, 24
  %sat_nonzero = icmp ne i32 %acc_sat_w32, 0
  %low16 = and i64 %w32_sext, 65535
  %63 = icmp ugt i64 %low16, 32768
  %64 = icmp eq i64 %low16, 32768
  %65 = and i64 %w32_sext, 65536
  %66 = icmp ne i64 %65, 0
  %67 = and i1 %64, %66
  %68 = or i1 %63, %67
  %69 = select i1 %68, i64 32768, i64 0
  %70 = add i64 %w32_sext, %69
  %sgnbits = and i64 %70, -281474976710656
  %71 = lshr i64 %70, 16
  %72 = or i64 %71, %sgnbits
  %low161 = and i64 %acc_add, 65535
  %73 = icmp ugt i64 %low161, 32768
  %74 = icmp eq i64 %low161, 32768
  %75 = and i64 %acc_add, 65536
  %76 = icmp ne i64 %75, 0
  %77 = and i1 %74, %76
  %78 = or i1 %73, %77
  %79 = select i1 %78, i64 32768, i64 0
  %80 = add i64 %acc_add, %79
  %sgnbits2 = and i64 %80, -281474976710656
  %81 = lshr i64 %80, 16
  %82 = or i64 %81, %sgnbits2
  %83 = icmp slt i64 %72, -32768
  %84 = icmp sgt i64 %72, 32767
  %85 = call i64 @llvm.smax.i64(i64 %72, i64 -32768)
  %86 = call i64 @llvm.smin.i64(i64 %85, i64 32767)
  %87 = or i1 %83, %84
  %ov_s16 = zext i1 %87 to i32
  %sat_s16 = trunc i64 %86 to i32
  %88 = icmp slt i64 %82, -32768
  %89 = icmp sgt i64 %82, 32767
  %90 = call i64 @llvm.smax.i64(i64 %82, i64 -32768)
  %91 = call i64 @llvm.smin.i64(i64 %90, i64 32767)
  %92 = or i1 %88, %89
  %ov_s163 = zext i1 %92 to i32
  %sat_s164 = trunc i64 %91 to i32
  %93 = zext i1 %sat_nonzero to i32
  %94 = and i32 %93, %ov_s163
  %ov16 = or i32 %ov_s16, %94
  %95 = getelementptr i8, ptr %shadow, i32 88
  %i_addr = load i32, ptr %95, align 4
  %guest_aligned = call i1 @bcore.guest.aligned(i32 %i_addr, i32 2)
  %96 = xor i1 %guest_aligned, true
  br i1 %96, label %data_misaligned, label %data_aligned

data_misaligned:                                  ; preds = %insn_0x00000104
  %97 = getelementptr i8, ptr %shadow, i64 0
  %98 = getelementptr i8, ptr %cpu, i64 0
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %98, ptr align 1 %97, i64 8, i1 false)
  %99 = getelementptr i8, ptr %shadow, i64 64
  %100 = getelementptr i8, ptr %cpu, i64 64
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %100, ptr align 1 %99, i64 4, i1 false)
  %101 = getelementptr i8, ptr %shadow, i64 72
  %102 = getelementptr i8, ptr %cpu, i64 72
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %102, ptr align 1 %101, i64 4, i1 false)
  %103 = getelementptr i8, ptr %shadow, i64 80
  %104 = getelementptr i8, ptr %cpu, i64 80
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %104, ptr align 1 %103, i64 4, i1 false)
  %105 = getelementptr i8, ptr %shadow, i64 88
  %106 = getelementptr i8, ptr %cpu, i64 88
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %106, ptr align 1 %105, i64 4, i1 false)
  %107 = getelementptr i8, ptr %shadow, i64 152
  %108 = getelementptr i8, ptr %cpu, i64 152
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %108, ptr align 1 %107, i64 4, i1 false)
  %109 = getelementptr i8, ptr %shadow, i64 188
  %110 = getelementptr i8, ptr %cpu, i64 188
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %110, ptr align 1 %109, i64 4, i1 false)
  %111 = getelementptr i8, ptr %shadow, i64 260
  %112 = getelementptr i8, ptr %cpu, i64 260
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %112, ptr align 1 %111, i64 8, i1 false)
  %113 = getelementptr i8, ptr %shadow, i64 292
  %114 = getelementptr i8, ptr %cpu, i64 292
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %114, ptr align 1 %113, i64 5, i1 false)
  %115 = getelementptr i8, ptr %shadow, i64 308
  %116 = getelementptr i8, ptr %cpu, i64 308
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %116, ptr align 1 %115, i64 4, i1 false)
  %117 = getelementptr i8, ptr %shadow, i64 348
  %118 = getelementptr i8, ptr %cpu, i64 348
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %118, ptr align 1 %117, i64 4, i1 false)
  call void null(ptr %cpu, ptr %mem, i32 260, i32 %i_addr, i32 0)
  %119 = getelementptr i8, ptr %shadow, i64 0
  %120 = getelementptr i8, ptr %cpu, i64 0
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %119, ptr align 1 %120, i64 8, i1 false)
  %121 = getelementptr i8, ptr %shadow, i64 64
  %122 = getelementptr i8, ptr %cpu, i64 64
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %121, ptr align 1 %122, i64 4, i1 false)
  %123 = getelementptr i8, ptr %shadow, i64 72
  %124 = getelementptr i8, ptr %cpu, i64 72
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %123, ptr align 1 %124, i64 4, i1 false)
  %125 = getelementptr i8, ptr %shadow, i64 80
  %126 = getelementptr i8, ptr %cpu, i64 80
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %125, ptr align 1 %126, i64 4, i1 false)
  %127 = getelementptr i8, ptr %shadow, i64 88
  %128 = getelementptr i8, ptr %cpu, i64 88
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %127, ptr align 1 %128, i64 4, i1 false)
  %129 = getelementptr i8, ptr %shadow, i64 112
  %130 = getelementptr i8, ptr %cpu, i64 112
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %129, ptr align 1 %130, i64 4, i1 false)
  %131 = getelementptr i8, ptr %shadow, i64 120
  %132 = getelementptr i8, ptr %cpu, i64 120
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %131, ptr align 1 %132, i64 4, i1 false)
  %133 = getelementptr i8, ptr %shadow, i64 152
  %134 = getelementptr i8, ptr %cpu, i64 152
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %133, ptr align 1 %134, i64 4, i1 false)
  %135 = getelementptr i8, ptr %shadow, i64 188
  %136 = getelementptr i8, ptr %cpu, i64 188
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %135, ptr align 1 %136, i64 4, i1 false)
  %137 = getelementptr i8, ptr %shadow, i64 260
  %138 = getelementptr i8, ptr %cpu, i64 260
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %137, ptr align 1 %138, i64 8, i1 false)
  %139 = getelementptr i8, ptr %shadow, i64 292
  %140 = getelementptr i8, ptr %cpu, i64 292
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %139, ptr align 1 %140, i64 5, i1 false)
  %141 = getelementptr i8, ptr %shadow, i64 308
  %142 = getelementptr i8, ptr %cpu, i64 308
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %141, ptr align 1 %142, i64 4, i1 false)
  %143 = getelementptr i8, ptr %shadow, i64 348
  %144 = getelementptr i8, ptr %cpu, i64 348
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %143, ptr align 1 %144, i64 4, i1 false)
  %145 = getelementptr i8, ptr %cpu, i64 64
  %146 = load i32, ptr %145, align 4
  %147 = getelementptr i8, ptr %cpu, i64 72
  %148 = load i32, ptr %147, align 4
  %149 = zext i32 %148 to i64
  %150 = trunc i32 %146 to i8
  %151 = sext i8 %150 to i64
  %152 = shl i64 %151, 32
  %153 = or i64 %152, %149
  %154 = getelementptr i8, ptr %shadow, i64 448
  store i64 %153, ptr %154, align 8
  %155 = getelementptr i8, ptr %shadow, i64 188
  %next_pc = load i32, ptr %155, align 4
  %156 = getelementptr i8, ptr %shadow, i64 308
  %packets = load i32, ptr %156, align 4
  br label %data_aligned

data_aligned:                                     ; preds = %data_misaligned, %insn_0x00000104
  %guest_ptr = call ptr @bcore.guest.ptr(ptr %page_table, i32 %i_addr)
  %page_mapped = icmp ne ptr %guest_ptr, null
  br label %page_fast

page_fast:                                        ; preds = %data_aligned
  %page_val = load i16, ptr %guest_ptr, align 2
  br label %page_done

page_done:                                        ; preds = %page_fast
  %val32 = zext i16 %page_val to i32
  %rd_hi = and i32 %s0_32, -65536
  %val_lo = and i32 %val32, 65535
  %new_rd = or i32 %rd_hi, %val_lo
  %157 = getelementptr i8, ptr %shadow, i32 120
  %breg = load i32, ptr %157, align 4
  %i64 = zext i32 %i_addr to i64
  %b64 = zext i32 %breg to i64
  %IM = add i64 %i64, 2
  %im32 = trunc i64 %IM to i32
  %LB = add i64 0, %b64
  %lb32 = trunc i64 %LB to i32
  %IML_pos = sub i64 %IM, 0
  %iml32_pos = trunc i64 %IML_pos to i32
  %IM_car = and i64 %IM, 4294967296
  %LB_car = and i64 %LB, 4294967296
  %same_carry = icmp eq i64 %IM_car, %LB_car
  %cmp_pos = icmp ult i32 %im32, %lb32
  %pos_same = select i1 %cmp_pos, i32 %im32, i32 %iml32_pos
  %pos_diff = select i1 %cmp_pos, i32 %iml32_pos, i32 %im32
  %pos_res = select i1 %same_carry, i32 %pos_same, i32 %pos_diff
  %IML_neg = add i64 %IM, 0
  %iml32_neg = trunc i64 %IML_neg to i32
  %i_msb_v = and i64 %i64, 2147483648
  %i_msb = icmp ne i64 %i_msb_v, 0
  %im_car_v = and i64 %IM, 4294967296
  %im_carry = icmp ne i64 %im_car_v, 0
  %flag_neg = or i1 %i_msb, %im_carry
  %cmp_neg = icmp ult i32 %im32, %breg
  %neg_flag = select i1 %cmp_neg, i32 %iml32_neg, i32 %im32
  %neg_noflag = select i1 %cmp_neg, i32 %im32, i32 %iml32_neg
  %neg_res = select i1 %flag_neg, i32 %neg_flag, i32 %neg_noflag
  %dag_result = select i1 false, i32 %neg_res, i32 %pos_res
  %158 = getelementptr i8, ptr %shadow, i32 80
  %i_addr5 = load i32, ptr %158, align 4
  %guest_aligned8 = call i1 @bcore.guest.aligned(i32 %i_addr5, i32 4)
  %159 = xor i1 %guest_aligned8, true
  br label %data_aligned7

data_aligned7:                                    ; preds = %page_done
  %guest_ptr12 = call ptr @bcore.guest.ptr(ptr %page_table, i32 %i_addr5)
  %page_mapped13 = icmp ne ptr %guest_ptr12, null
  br label %page_fast9

page_fast9:                                       ; preds = %data_aligned7
  %page_val14 = load i32, ptr %guest_ptr12, align 4
  br label %page_done11

page_done11:                                      ; preds = %page_fast9
  %160 = getelementptr i8, ptr %shadow, i32 112
  %breg23 = load i32, ptr %160, align 4
  %i6424 = zext i32 %i_addr5 to i64
  %b6425 = zext i32 %breg23 to i64
  %IM26 = add i64 %i6424, 4
  %im3227 = trunc i64 %IM26 to i32
  %LB28 = add i64 0, %b6425
  %lb3229 = trunc i64 %LB28 to i32
  %IML_pos30 = sub i64 %IM26, 0
  %iml32_pos31 = trunc i64 %IML_pos30 to i32
  %IM_car32 = and i64 %IM26, 4294967296
  %LB_car33 = and i64 %LB28, 4294967296
  %same_carry34 = icmp eq i64 %IM_car32, %LB_car33
  %cmp_pos35 = icmp ult i32 %im3227, %lb3229
  %pos_same36 = select i1 %cmp_pos35, i32 %im3227, i32 %iml32_pos31
  %pos_diff37 = select i1 %cmp_pos35, i32 %iml32_pos31, i32 %im3227
  %pos_res38 = select i1 %same_carry34, i32 %pos_same36, i32 %pos_diff37
  %IML_neg39 = add i64 %IM26, 0
  %iml32_neg40 = trunc i64 %IML_neg39 to i32
  %i_msb_v41 = and i64 %i6424, 2147483648
  %i_msb42 = icmp ne i64 %i_msb_v41, 0
  %im_car_v43 = and i64 %IM26, 4294967296
  %im_carry44 = icmp ne i64 %im_car_v43, 0
  %flag_neg45 = or i1 %i_msb42, %im_carry44
  %cmp_neg46 = icmp ult i32 %im3227, %breg23
  %neg_flag47 = select i1 %cmp_neg46, i32 %iml32_neg40, i32 %im3227
  %neg_noflag48 = select i1 %cmp_neg46, i32 %im3227, i32 %iml32_neg40
  %neg_res49 = select i1 %flag_neg45, i32 %neg_flag47, i32 %neg_noflag48
  %dag_result50 = select i1 false, i32 %neg_res49, i32 %pos_res38
  %161 = getelementptr i8, ptr %shadow, i32 0
  store i32 %page_val14, ptr %161, align 4
  %162 = getelementptr i8, ptr %shadow, i32 4
  store i32 %new_rd, ptr %162, align 4
  %163 = getelementptr i8, ptr %shadow, i32 64
  store i32 %ax_8, ptr %163, align 4
  %ax8 = trunc i32 %ax_8 to i8
  %ax64 = sext i8 %ax8 to i64
  %shifted = shl i64 %ax64, 32
  %aw64 = zext i32 %aw to i64
  %acc51 = or i64 %shifted, %aw64
  %acc_slot52 = getelementptr i8, ptr %shadow, i64 448
  store i64 %acc51, ptr %acc_slot52, align 8
  %164 = getelementptr i8, ptr %shadow, i32 72
  store i32 %aw_new, ptr %164, align 4
  %ax853 = trunc i32 %ax_8 to i8
  %ax6454 = sext i8 %ax853 to i64
  %shifted55 = shl i64 %ax6454, 32
  %aw6456 = zext i32 %aw_new to i64
  %acc57 = or i64 %shifted55, %aw6456
  %acc_slot58 = getelementptr i8, ptr %shadow, i64 448
  store i64 %acc57, ptr %acc_slot58, align 8
  %165 = getelementptr i8, ptr %shadow, i32 80
  store i32 %dag_result50, ptr %165, align 4
  %166 = getelementptr i8, ptr %shadow, i32 88
  store i32 %dag_result, ptr %166, align 4
  %167 = getelementptr i8, ptr %shadow, i32 260
  store i32 %acc_sat_w32, ptr %167, align 4
  %168 = getelementptr i8, ptr %shadow, i32 264
  store i32 %60, ptr %168, align 4
  %acc_slot59 = getelementptr i8, ptr %shadow, i64 448
  store i64 %acc40, ptr %acc_slot59, align 8
  %169 = getelementptr i8, ptr %shadow, i32 152
  %lc0 = load i32, ptr %169, align 4
  %170 = icmp ugt i32 %lc0, 1
  br label %hwloop_back

hwloop_back:                                      ; preds = %page_done11
  %171 = getelementptr i8, ptr %shadow, i32 152
  %172 = sub i32 %lc0, 1
  store i32 %172, ptr %171, align 4
  %173 = getelementptr i8, ptr %shadow, i64 308
  %packets72 = load i32, ptr %173, align 4
  %174 = getelementptr i8, ptr %shadow, i64 348
  %chain_packets = load i32, ptr %174, align 4
  %chain_total = add nuw i32 %chain_packets, %packets72
  %175 = icmp ult i32 %chain_total, %chain_budget
  br i1 %175, label %chain_budget_now, label %chain_leave73

chain_go:                                         ; preds = %chain_budget_now
  %176 = getelementptr i8, ptr %shadow, i64 188
  store i32 260, ptr %176, align 4
  %177 = getelementptr i8, ptr %shadow, i64 348
  store i32 %chain_total, ptr %177, align 4
  br label %insn_0x00000104_entry

chain_leave73:                                    ; preds = %chain_budget_now, %hwloop_back
  %178 = getelementptr i8, ptr %shadow, i64 188
  store i32 260, ptr %178, align 4
  %179 = getelementptr i8, ptr %shadow, i64 296
  store i8 1, ptr %179, align 1
  %180 = getelementptr i8, ptr %shadow, i64 0
  %181 = getelementptr i8, ptr %cpu, i64 0
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %181, ptr align 1 %180, i64 8, i1 false)
  %182 = getelementptr i8, ptr %shadow, i64 64
  %183 = getelementptr i8, ptr %cpu, i64 64
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %183, ptr align 1 %182, i64 4, i1 false)
  %184 = getelementptr i8, ptr %shadow, i64 72
  %185 = getelementptr i8, ptr %cpu, i64 72
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %185, ptr align 1 %184, i64 4, i1 false)
  %186 = getelementptr i8, ptr %shadow, i64 80
  %187 = getelementptr i8, ptr %cpu, i64 80
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %187, ptr align 1 %186, i64 4, i1 false)
  %188 = getelementptr i8, ptr %shadow, i64 88
  %189 = getelementptr i8, ptr %cpu, i64 88
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %189, ptr align 1 %188, i64 4, i1 false)
  %190 = getelementptr i8, ptr %shadow, i64 152
  %191 = getelementptr i8, ptr %cpu, i64 152
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %191, ptr align 1 %190, i64 4, i1 false)
  %192 = getelementptr i8, ptr %shadow, i64 188
  %193 = getelementptr i8, ptr %cpu, i64 188
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %193, ptr align 1 %192, i64 4, i1 false)
  %194 = getelementptr i8, ptr %shadow, i64 260
  %195 = getelementptr i8, ptr %cpu, i64 260
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %195, ptr align 1 %194, i64 8, i1 false)
  %196 = getelementptr i8, ptr %shadow, i64 292
  %197 = getelementptr i8, ptr %cpu, i64 292
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %197, ptr align 1 %196, i64 5, i1 false)
  %198 = getelementptr i8, ptr %shadow, i64 308
  %199 = getelementptr i8, ptr %cpu, i64 308
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %199, ptr align 1 %198, i64 4, i1 false)
  %200 = getelementptr i8, ptr %shadow, i64 348
  %201 = getelementptr i8, ptr %cpu, i64 348
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %201, ptr align 1 %200, i64 4, i1 false)
  ret void

chain_budget_now:                                 ; preds = %hwloop_back
  %202 = getelementptr i8, ptr %cpu, i64 344
  %chain_budget74 = load atomic i32, ptr %202 monotonic, align 4
  %203 = icmp ne i32 %chain_budget74, 0
  br i1 %203, label %chain_go, label %chain_leave73
}

; Function Attrs: nocallback nofree nosync nounwind readnone speculatable willreturn
declare i64 @llvm.smax.i64(i64, i64) #1

; Function Attrs: nocallback nofree nosync nounwind readnone speculatable willreturn
declare i64 @llvm.smin.i64(i64, i64) #1

; Function Attrs: nounwind readnone willreturn
declare i1 @bcore.guest.aligned(i32, i32) #2

; Function Attrs: nounwind readnone willreturn
declare ptr @bcore.guest.ptr(ptr, i32) #2

; Function Attrs: argmemonly nocallback nofree nounwind willreturn
declare void @llvm.memcpy.p0.p0.i64(ptr noalias nocapture writeonly, ptr noalias nocapture readonly, i64, i1 immarg) #3

attributes #0 = { "bcore-loop-block" "no_callee_saved_registers" }
attributes #1 = { nocallback nofree nosync nounwind readnone speculatable willreturn }
attributes #2 = { nounwind readnone willreturn }
attributes #3 = { argmemonly nocallback nofree nounwind willreturn }
