// Blocks compiled for WebAssembly (Compiler::set_target_triple) on any host
// whose LLVM has the target: in the blocks' convention there (block_abi.h:
// C's, each exported under its name), and objects the WebAssembly loader
// runs as they are (wasm_object.h). And objects it must refuse: data,
// a function's place in the table, an import it does not give.
#include "core.h"
#include "../src/bb_translator.h"
#include "../src/block_abi.h"
#include "../src/compiler.h"
#include "../src/helper_abi.h"
#include "../src/helper_functions.h"
#include "../src/wasm_object.h"
#include "../tools/flat_memory.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetSelect.h>

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace bcore;

static constexpr const char* kTriple = "wasm32-unknown-emscripten";

static void require(bool ok, const std::string& why) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", why.c_str());
        std::exit(1);
    }
}

// What the loader gives a block's module (code_loader_wasm.cpp).
static std::vector<WasmImport> loader_imports() {
    std::vector<WasmImport> imports = {{"__linear_memory", WasmImportKind::Memory},
                                       {"__indirect_function_table", WasmImportKind::Table},
                                       {"__stack_pointer", WasmImportKind::Global}};
    for (const auto& [name, fn] : bcore_abi::helper_functions()) imports.push_back({name, WasmImportKind::Function});
    for (const char* name : {"memset", "memcpy", "memmove"}) imports.push_back({name, WasmImportKind::Function});
    return imports;
}

static std::string check(const llvm::MemoryBuffer& object, const std::string& name) {
    const auto imports = loader_imports();
    return check_wasm_object(object.getMemBufferRef(), imports.data(), imports.size(), name);
}

// The block at pc of mem's code, lifted for WebAssembly, optimized and
// emitted at opt: its object must run as it is.
static void block(Compiler& compiler, FlatMemory& mem, uint32_t pc, bool chained, const char* what) {
    BBTranslator translator(&mem);
    translator.setModuleTarget(compiler.dataLayout(), compiler.targetTriple());
    translator.set_chaining(chained, false);
    BBTranslateResult result = translator.translate(pc);
    require(result.module != nullptr, std::string(what) + ": lifted");
    llvm::Module& mod = *result.module;
    require(bcore_abi::is_wasm(mod), std::string(what) + ": a module for WebAssembly");
    for (llvm::Function& fn : mod) {
        if (fn.isDeclaration()) {
            if (bcore_abi::find_helper(fn.getName()))
                require(fn.getCallingConv() == llvm::CallingConv::C, std::string(what) + ": a helper in C's convention");
            continue;
        }
        require(fn.getCallingConv() == llvm::CallingConv::C, std::string(what) + ": a block in C's convention");
        require(!fn.hasParamAttribute(bcore_abi::kCpuArg, llvm::Attribute::SwiftSelf) &&
                    !fn.hasFnAttribute(bcore_abi::kNoCalleeSaved),
                std::string(what) + ": no swiftself, no no_callee_saved_registers");
        require(fn.getFnAttribute("wasm-export-name").getValueAsString() == fn.getName(),
                std::string(what) + ": exported under its name");
    }
    const std::string name = mod.begin()->getName().str();
    compiler.optimize(mod);
    auto object = compiler.emit(mod);
    require(object != nullptr, std::string(what) + ": emitted");
    const std::string why = check(*object, name);
    require(why.empty(), std::string(what) + ": its object has " + why);
}

// A module of one exported function `f`, its body made by body, emitted.
template <typename Body>
static std::unique_ptr<llvm::MemoryBuffer> module(Compiler& compiler, Body body) {
    llvm::LLVMContext ctx;
    llvm::Module mod("m", ctx);
    mod.setDataLayout(compiler.dataLayout());
    set_target_triple(mod, compiler.targetTriple());
    auto* fn = bcore_abi::make_block(mod, "f");
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    body(mod, b, fn);
    b.CreateRetVoid();
    return compiler.emit(mod);
}

// Refused, for one of reasons (whichever the object has first).
static void refused(const std::unique_ptr<llvm::MemoryBuffer>& object, std::initializer_list<const char*> reasons,
                    const char* what) {
    require(object != nullptr, std::string(what) + ": emitted");
    const std::string why = check(*object, "f");
    bool expected = false;
    for (const char* reason : reasons) expected |= why.find(reason) != std::string::npos;
    require(expected, std::string(what) + ": refused for " + *reasons.begin() + ", not for \"" + why + "\"");
}

int main() {
    LLVMInitializeWebAssemblyTargetInfo();
    LLVMInitializeWebAssemblyTarget();
    LLVMInitializeWebAssemblyTargetMC();
    LLVMInitializeWebAssemblyAsmPrinter();

    for (int opt : {0, 1, 2}) {
        Compiler compiler;
        compiler.set_target_triple(kTriple);
        require(compiler.init(opt, -1), "a compiler for WebAssembly");
        require(!compiler.split(), "no splitter on WebAssembly");
        CpuState cpu{};
        FlatMemory mem(0, 0x1000, &cpu);
        // R0 += 1 six times, JUMP.S 0x100: straight-line code, and with
        // chaining the next block run through the table (return_call_indirect).
        for (int i = 0; i < 6; ++i) mem.write16(0x100 + 2 * i, 0x6408);
        mem.write16(0x10c, 0x2000);
        // R0 = [P0]; [P0++] = R0; JUMP.S 0x300: the memory's helpers.
        mem.write16(0x200, 0x9100);
        mem.write16(0x202, 0x9200);
        mem.write16(0x204, 0x207e);
        for (bool chained : {false, true}) {
            block(compiler, mem, 0x100, chained, chained ? "chained straight-line block" : "straight-line block");
            block(compiler, mem, 0x200, chained, chained ? "chained block with loads and stores" : "block with loads and stores");
        }
        // The same compiler, a second module: its exports too (a backend for
        // each module, Compiler::emit).
        block(compiler, mem, 0x100, true, "a later module of the same compiler");

        // A table of constants indexed at run time: data.
        refused(module(compiler, [](llvm::Module& mod, llvm::IRBuilder<>& b, llvm::Function* fn) {
                    auto* i32 = b.getInt32Ty();
                    auto* type = llvm::ArrayType::get(i32, 4);
                    auto* table = new llvm::GlobalVariable(
                        mod, type, true, llvm::GlobalValue::InternalLinkage,
                        llvm::ConstantArray::get(type, {b.getInt32(3), b.getInt32(5), b.getInt32(7), b.getInt32(11)}),
                        "table");
                    auto* index = b.CreateAnd(b.CreateLoad(i32, bcore_abi::block_cpu(fn)), b.getInt32(3));
                    auto* value = b.CreateLoad(i32, b.CreateGEP(type, table, {b.getInt32(0), index}));
                    b.CreateStore(value, bcore_abi::block_cpu(fn));
                }),
                {"DATA"}, "a module with data");
        // A function's own address stored: its place in the table, which a
        // linker would give it (and LLVM 23 puts it in an element segment).
        refused(module(compiler, [](llvm::Module&, llvm::IRBuilder<>& b, llvm::Function* fn) {
                    b.CreateStore(fn, bcore_abi::block_mem(fn));
                }),
                {"R_WASM_TABLE_INDEX", "ELEM"}, "a module that takes a function's address");
        // A call of a function the loader does not give.
        refused(module(compiler, [](llvm::Module& mod, llvm::IRBuilder<>& b, llvm::Function*) {
                    auto callee = mod.getOrInsertFunction("bcore_unknown", b.getVoidTy());
                    b.CreateCall(callee);
                }),
                {"an import of function env.bcore_unknown"}, "a module that imports what the loader does not give");
    }
    printf("wasm object test passed\n");
    return 0;
}
