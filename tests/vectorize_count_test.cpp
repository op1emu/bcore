// count_module's vectorized loops: a speculative copy (guest_speculation.h)
// counts once, however many vector loops the vectorizer made of it.
//
//   bcore-vectorize-count-test rules
//     fixed IR: one copy's main vector loop and vectorized epilogue (one
//     tag), another copy (its own tag), scalar remainders, an untagged
//     vector loop, and a loop with vector work the vectorizer never marked.
//   bcore-vectorize-count-test pipeline FILE
//     a loop block's module as Compiler::optimize is given it (a W32 MAC
//     loop, tests/data/), compiled -- never run -- at O1 and O2 for Zen 4 as
//     the LLVM in use sees it (a vector loop and a vectorized epilogue) and
//     for x86-64-v3 (VF 8, none). Each: one loop speculated, one counted
//     vectorized, and as many vector loops as that CPU's vectorizer makes.
//     This is what failed on Zen 4 runners only, now on any x86-64 host.

#include "../src/compiler.h"
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {

void require(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        exit(1);
    }
}

// Latches marked llvm.loop.isvectorized that do vector work: what
// count_module counted before speculative copies counted once.
unsigned vector_loops(const llvm::Module& mod) {
    unsigned n = 0;
    for (const llvm::Function& f : mod)
        for (const llvm::BasicBlock& block : f) {
            auto* loop = block.getTerminator() ? block.getTerminator()->getMetadata(llvm::LLVMContext::MD_loop) : nullptr;
            if (!loop) continue;
            bool vectorized = false, vector = false;
            for (auto& op : loop->operands())
                if (auto* node = llvm::dyn_cast<llvm::MDNode>(op.get()); node && node->getNumOperands() &&
                    llvm::isa<llvm::MDString>(node->getOperand(0)) &&
                    llvm::cast<llvm::MDString>(node->getOperand(0))->getString() == "llvm.loop.isvectorized")
                    vectorized = true;
            for (auto& inst : block) vector |= inst.getType()->isVectorTy();
            n += vectorized && vector;
        }
    return n;
}

// Each loop a block that branches back to itself: its vector work an add,
// its marks the latch's !llvm.loop.
const char* const kRulesIR = R"(
define void @f(<4 x i32> %v, i32 %s, i1 %c) {
main:                                       ; copy A's main vector loop
  %a0 = add <4 x i32> %v, %v
  br i1 %c, label %main, label %epilogue, !llvm.loop !0
epilogue:                                   ; copy A's vectorized epilogue
  %a1 = add <4 x i32> %v, %v
  br i1 %c, label %epilogue, label %scalar, !llvm.loop !1
scalar:                                     ; copy A's scalar remainder
  %a2 = add i32 %s, %s
  br i1 %c, label %scalar, label %other, !llvm.loop !2
other:                                      ; copy B's vector loop
  %b0 = add <4 x i32> %v, %v
  br i1 %c, label %other, label %other.scalar, !llvm.loop !3
other.scalar:                               ; copy B's scalar remainder
  %b1 = add i32 %s, %s
  br i1 %c, label %other.scalar, label %plain, !llvm.loop !4
plain:                                      ; vectorized, no copy's
  %p0 = add <4 x i32> %v, %v
  br i1 %c, label %plain, label %unmarked, !llvm.loop !5
unmarked:                                   ; vector work, never vectorized
  %u0 = add <4 x i32> %v, %v
  br i1 %c, label %unmarked, label %done, !llvm.loop !6
done:
  ret void
}
!0 = distinct !{!0, !10, !20}
!1 = distinct !{!1, !10, !20, !21}
!2 = distinct !{!2, !10, !20}
!3 = distinct !{!3, !11, !20}
!4 = distinct !{!4, !11, !20}
!5 = distinct !{!5, !20}
!6 = distinct !{!6, !10}
!10 = distinct !{!"bcore.speculated"}
!11 = distinct !{!"bcore.speculated"}
!20 = !{!"llvm.loop.isvectorized", i32 1}
!21 = !{!"llvm.loop.unroll.runtime.disable"}
)";

void rules() {
    llvm::LLVMContext context;
    llvm::SMDiagnostic diag;
    auto mod = llvm::parseIR(llvm::MemoryBufferRef(kRulesIR, "rules"), diag, context);
    require(mod != nullptr, "the rules' IR parses");
    require(vector_loops(*mod) == 4, "the rules' IR has four vector loops by the old count");
    const ModuleCounts counts = count_module(*mod);
    require(counts.loops_vectorized == 3, "copy A once, copy B once, the untagged loop once");
    printf("rules: %u vector loops, %llu counted vectorized\n", vector_loops(*mod),
           (unsigned long long)counts.loops_vectorized);
}

struct Target {
    const char* name;
    const char* cpu;
    const char* features;
    unsigned vector_loops;  // the main loop, and an epilogue from VF 16 on
};

// What sys::getHostCPUName and getHostCPUFeatures report on Zen 4 (AMD EPYC
// 9V74): to LLVM 15 znver3, whose tuning does not prefer 256-bit vectors,
// with every AVX-512 extension it has (VF 16); to LLVM 19 znver4, with its
// default features. (For LLVM 15's znver3 LLVM 19 makes one VF 8 loop and no
// epilogue.)
constexpr Target kTargets[] = {
#if LLVM_VERSION_MAJOR >= 19
    {"zen4", "znver4", "", 2},
#else
    {"zen4", "znver3",
     "+avx512f,+avx512dq,+avx512cd,+avx512bw,+avx512vl,+avx512ifma,+avx512vbmi,+avx512vbmi2,+avx512vnni,"
     "+avx512bitalg,+avx512vpopcntdq,+avx512bf16",
     2},
#endif
    {"x86-64-v3", "x86-64-v3", "", 1},
};

void pipeline(const char* path) {
    for (int opt : {1, 2})
        for (const Target& target : kTargets) {
            llvm::LLVMContext context;
            llvm::SMDiagnostic diag;
            auto mod = llvm::parseIRFile(path, diag, context);
            if (!mod) diag.print("bcore-vectorize-count-test", llvm::errs());
            require(mod != nullptr, "the module parses");
            Compiler compiler;
            compiler.set_target_cpu(target.cpu, target.features);
            require(compiler.init(opt, -1, false), "the compiler initializes");
            compiler.optimize(*mod);
            const ModuleCounts counts = count_module(*mod);
            const unsigned loops = vector_loops(*mod);
            printf("pipeline -O%d %s: %llu speculated, %u vector loops, %llu counted vectorized\n", opt, target.name,
                   (unsigned long long)counts.loops_speculated, loops, (unsigned long long)counts.loops_vectorized);
            require(counts.loops_speculated == 1, "the MAC loop is speculated");
            require(loops == target.vector_loops, "the CPU's vectorizer makes the vector loops expected");
            require(counts.loops_vectorized == 1, "the speculated loop counts as vectorized once");
        }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "rules")) {
        rules();
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "pipeline")) {
        pipeline(argv[2]);
        return 0;
    }
    fprintf(stderr, "usage: %s rules | pipeline FILE\n", argv[0]);
    return 2;
}
