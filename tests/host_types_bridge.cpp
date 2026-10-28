// bcore's side of bcore-host-types-test (host_types_test.cpp, whose global
// types are named as bcore's): a loop block's module optimizes at O1 and
// O2 (GuestLoopVersioning's std::vector<Access>), and the JIT finds the
// block it was given (JitEngine's std::unordered_map<uint32_t, BBInfo>).

#include "../src/compiler.h"
#include "../src/jit_engine.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace {

void require(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        exit(1);
    }
}

// Never run: the JIT only keeps its address.
void block(bcore::CpuState*, bcore::Memory*) {}

}  // namespace

int run_bcore(const char* file) {
    for (int opt : {1, 2}) {
        llvm::LLVMContext context;
        llvm::SMDiagnostic diag;
        auto mod = llvm::parseIRFile(file, diag, context);
        if (!mod) diag.print("bcore-host-types-test", llvm::errs());
        require(mod != nullptr, "the module parses");
        bcore::Compiler compiler;
        require(compiler.init(opt, -1, false), "the compiler initializes");
        try {
            compiler.optimize(*mod);
        } catch (const std::exception& e) {
            fprintf(stderr, "FAIL: -O%d: the loop block's module threw %s\n", opt, e.what());
            return 1;
        }
        printf("-O%d: the loop block's module optimizes\n", opt);
    }

    bcore::JitEngine engine;
    require(engine.init(), "the JIT initializes");
    engine.setChainFilter([](uint32_t) { return false; });
    for (uint32_t pc = 0x100; pc < 0x200; pc += 2) engine.addCode(pc, &block, pc + 2);
    for (uint32_t pc = 0x100; pc < 0x200; pc += 2)
        require(engine.lookup(pc) == &block, "the JIT finds the block it was given");
    printf("the JIT finds its blocks\n");
    return 0;
}
