// Compiled blocks share one arena (CodeArena): their code lies next to each
// other, not each at the start of pages of its own, and a new engine
// (Core::invalidate) gets a new arena that works the same. An object that no
// longer fits goes whole into a new arena, and one no arena can take into a
// memory manager of its own: a full arena never stops the process.
#include "core.h"
#include "../src/cec.h"
#include "../src/code_arena.h"
#include "../src/evt.h"
#include "../src/jit_engine.h"
#include "../tools/flat_memory.h"

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <string>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

static void require(bool ok, const char* why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

static constexpr int kBlocks = 64;
static constexpr uint32_t kStride = 0x10;

// Block i: R1 = i; JUMP.S 0 (to itself: one block per run, chaining is off).
static void write_blocks(FlatMemory& mem) {
    for (int i = 0; i < kBlocks; ++i) {
        const uint32_t pc = 0x100 + i * kStride;
        mem.write16(pc, uint16_t(0x6000 | (i << 3) | 1));
        mem.write16(pc + 2, 0x2000);
    }
}

static void run_all(Core& core, CpuState& cpu) {
    for (int i = 0; i < kBlocks; ++i) {
        require(core.run(0x100 + i * kStride), "run");
        require(cpu.dpregs[1] == uint32_t(i), "R1 = i");
    }
}

// The code map lists a block's function and, where the machine function
// splitter made one, its cold part (with profiling on; without, the
// functions only). The functions lie together; cold parts lie apart from
// them, in the arena's cold region.
static void require_packed(const Core& core, int opt) {
    std::map<uint32_t, std::vector<uintptr_t>> by_pc;
    core.forEachCompiledBlock([&](const BcoreBlockInfo& b) {
        by_pc[b.guest_pc].push_back(reinterpret_cast<uintptr_t>(b.host_addr));
    });
    require(by_pc.size() == size_t(kBlocks), "every block compiled");
    std::set<uintptr_t> pages;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    bool unaligned = false;
    std::vector<uintptr_t> cold;
    for (auto& [pc, addrs] : by_pc) {
        require(addrs.size() <= 2, "a function and at most one cold part");
        std::sort(addrs.begin(), addrs.end());
        const uintptr_t a = addrs[0];
        pages.insert(a >> 12);
        lo = std::min(lo, a);
        hi = std::max(hi, a);
        unaligned |= (a & 0xfff) != 0;
        if (addrs.size() == 2) cold.push_back(addrs[1]);
    }
    require(unaligned, "blocks do not all start a page");
    require(pages.size() < by_pc.size() / 4, "blocks share pages");
    require(hi - lo < (uintptr_t(1) << 20), "blocks lie together");
    // A jump's rarely taken loop-bottom path is cold in every one of them;
    // the backend splits from CodeGenOpt::Less up.
    if (opt > 0 && core.profileHasBlockSizes()) require(cold.size() == size_t(kBlocks), "the splitter made cold parts");
    for (uintptr_t c : cold) require(c - lo >= (uintptr_t(256) << 20), "cold parts lie apart");
}

static void test(int opt) {
    CpuState cpu{};
    FlatMemory mem(0, 0x1000, &cpu);
    Core core(&cpu, &mem);
    require(core.init(opt), "init");
    cec_init(); evt_init();
    write_blocks(mem);
    run_all(core, cpu);
    require_packed(core, opt);
    run_all(core, cpu);  // compiled code runs again from the arena

    core.invalidate();
    run_all(core, cpu);
    require_packed(core, opt);
}

static constexpr size_t kHalf = size_t(256) << 10;  // hot 192 KiB, cold 64 KiB, data 256 KiB

static uintptr_t addr(const void* p) { return reinterpret_cast<uintptr_t>(p); }

// Objects of one shape, placed until two arenas are full and beyond: every
// section is given, an object's sections lie in one arena, a new arena is
// mapped when one is full, and past the pool's limit an object gets its own
// manager. hot/cold/data are the bytes each object asks for in that region.
static void exhaust(const char* what, uintptr_t hot, uintptr_t cold, uintptr_t data) {
    auto pool = std::make_shared<CodeArenaPool>(kHalf, 2);
    std::vector<std::unique_ptr<ArenaMemoryManager>> objects;
    size_t own = 0;
    for (int i = 0; i < 64; ++i) {
        auto mm = std::make_unique<ArenaMemoryManager>(pool);
        mm->reserveAllocationSpace(hot + cold, 16, data, 16, 0, 1);
        uint8_t* h = mm->allocateCodeSection(hot, 16, 1, ".text");
        uint8_t* c = cold ? mm->allocateCodeSection(cold, 16, 2, ".text.split.f") : h;
        uint8_t* d = data ? mm->allocateDataSection(data, 16, 3, ".rodata", true) : h;
        require(h && c && d, what);
        std::string error;
        require(!mm->finalizeMemory(&error), what);
        if (!mm->inArena()) { ++own; continue; }
        const uintptr_t lo = std::min({addr(h), addr(c), addr(d)}), hi = std::max({addr(h), addr(c), addr(d)});
        require(hi - lo < 2 * kHalf, what);  // one arena
        objects.push_back(std::move(mm));
    }
    require(pool->arenas() == 2 && own > 0, what);
}

// An object larger than an empty arena never takes one.
static void too_large() {
    auto pool = std::make_shared<CodeArenaPool>(kHalf, 2);
    ArenaMemoryManager mm(pool);
    mm.reserveAllocationSpace(kHalf, 16, 0, 1, 0, 1);
    require(mm.allocateCodeSection(kHalf, 16, 1, ".text") && !mm.inArena() && pool->arenas() == 0,
            "an object larger than an arena gets its own manager");
}

// The same through LLJIT: functions of ~3 KB each, compiled into arenas of
// 192 KiB of hot code until two are full and past them, all still run.
static void jit_exhaust() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    auto pool = std::make_shared<CodeArenaPool>(kHalf, 2);
    auto jit = llvm::orc::LLJITBuilder()
        .setObjectLinkingLayerCreator([pool](llvm::orc::ExecutionSession& es, const llvm::Triple&)
                                          -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
            return std::make_unique<llvm::orc::RTDyldObjectLinkingLayer>(
                es, [pool] { return std::make_unique<ArenaMemoryManager>(pool); });
        })
        .create();
    require(bool(jit), "LLJIT");
    static volatile int32_t sink;
    for (int i = 0; i < 200; ++i) {
        auto ctx = std::make_unique<llvm::LLVMContext>();
        auto mod = std::make_unique<llvm::Module>("m", *ctx);
        mod->setDataLayout((*jit)->getDataLayout());
        const std::string name = "f" + std::to_string(i);
        auto* fn = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getInt32Ty(*ctx), {llvm::Type::getInt8PtrTy(*ctx)}, false),
            llvm::Function::ExternalLinkage, name, mod.get());
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(*ctx, "entry", fn));
        for (int k = 0; k < 300; ++k) b.CreateStore(b.getInt32(k), fn->getArg(0), /*isVolatile=*/true);
        b.CreateRet(b.getInt32(i));
        require(!(*jit)->addIRModule(llvm::orc::ThreadSafeModule(std::move(mod), std::move(ctx))), "add module");
        auto sym = (*jit)->lookup(name);
        require(bool(sym), "lookup");
        auto* f = reinterpret_cast<int32_t (*)(volatile int32_t*)>(sym->getValue());
        require(f(&sink) == i && sink == 299, "a function placed anywhere runs");
    }
    require(pool->arenas() == 2, "two arenas, then managers of their own");
}

// Code placed in an engine's arena directly (JitEngine::codeArena) stays
// mapped as long as the engine, when nothing else holds the arena any more
// and the pool has moved on to another (found in review: such code went
// with the copy-and-patch translator that had held its arena).
static void placed_code_lives() {
    JitEngine jit;
    require(jit.init(), "JitEngine init");
    // Fill the hot region (address space only: nothing is touched), so the
    // next request maps another arena.
    const size_t rest = CodeArena::kDefaultHalf / 4 * 3 - (64 << 10);
    auto first = jit.codeArena(64);  // the engine's own code (bcore_enter) holds this one
    require(first && first->allocate(CodeArena::Region::Hot, rest, 16) != nullptr, "most of the first arena");
    auto second = jit.codeArena(128 << 10);
    require(second && second != first, "a second arena");
    uint8_t* code = second->allocate(CodeArena::Region::Hot, 64, 16);
    require(code && second->allocate(CodeArena::Region::Hot, rest, 16) != nullptr, "most of the second arena");
    auto third = jit.codeArena(128 << 10);
    require(third && third != second, "a third arena");
    first.reset();
    second.reset();
    third.reset();
    const size_t page = size_t(sysconf(_SC_PAGESIZE));
    unsigned char resident;
    void* at = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(code) & ~(page - 1));
    require(mincore(at, page, &resident) == 0, "the second arena's code is still mapped");
}

int main() {
    for (int opt = 0; opt <= 2; ++opt) test(opt);
    placed_code_lives();
    exhaust("hot code fills an arena", 20 << 10, 0, 1 << 10);
    exhaust("cold code fills an arena", 1 << 10, 20 << 10, 1 << 10);
    exhaust("data fills an arena", 1 << 10, 0, 40 << 10);
    too_large();
    jit_exhaust();
    printf("code arena test passed\n");
    return 0;
}
