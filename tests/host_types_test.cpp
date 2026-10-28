// A host's own types must not reach into bcore's: the library keeps its
// file-local helper types in anonymous namespaces. This host defines a
// global `struct Access` and `struct Tail` of other layouts, as a host may,
// and uses std::vector<Access> as bcore's GuestLoopVersioning does. Were
// bcore's types global too, the two would share one copy of each vector
// member (the ODR), and the pass would grow its access list with this
// one's element size: a loop block at -O1 threw std::length_error.
//
//   bcore-host-types-test FILE    a loop block's module (tests/data/), optimized at O1 and O2

#include "../src/compiler.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <vector>

// The host's types: global, and sized unlike bcore's.
struct Access {
    uint32_t addr, value, pc, packets;
    uint8_t width, write;
};
struct Tail {
    uint64_t value[3] = {};
    bool any() const { return value[0] != 0; }
};

namespace {

void require(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        exit(1);
    }
}

// Uses the host's types the way bcore uses its own, so this binary has its
// own copies of the members a clash would share.
volatile uint32_t sink;
void use_host_types() {
    std::vector<Access> trace;
    for (uint32_t i = 0; i < 64; ++i) trace.push_back({i, i, i, i, 4, 0});
    Tail tail;
    tail.value[0] = trace.size();
    sink = uint32_t(trace.size()) + tail.any();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s FILE\n", argv[0]);
        return 2;
    }
    use_host_types();
    for (int opt : {1, 2}) {
        llvm::LLVMContext context;
        llvm::SMDiagnostic diag;
        auto mod = llvm::parseIRFile(argv[1], diag, context);
        if (!mod) diag.print("bcore-host-types-test", llvm::errs());
        require(mod != nullptr, "the module parses");
        Compiler compiler;
        require(compiler.init(opt, -1, false), "the compiler initializes");
        try {
            compiler.optimize(*mod);
        } catch (const std::exception& e) {
            fprintf(stderr, "FAIL: -O%d: the loop block's module threw %s\n", opt, e.what());
            return 1;
        }
        printf("-O%d: the loop block's module optimizes\n", opt);
    }
    return 0;
}
