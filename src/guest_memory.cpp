#include "guest_memory.h"

#include <set>
#include <string>
#include <vector>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ScalarEvolutionExpander.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "mem.h"
#include "scev_expander.h"

namespace bcore {

using namespace llvm;

static constexpr const char* kGuestPtr = "bcore.guest.ptr";
static constexpr const char* kGuestPtrIn = "bcore.guest.ptr.in";
static constexpr const char* kGuestAligned = "bcore.guest.aligned";

static Function* declare(Module& m, const char* name, FunctionType* type) {
    auto* f = cast<Function>(m.getOrInsertFunction(name, type).getCallee());
    f->setDoesNotAccessMemory();
    f->setDoesNotThrow();
    f->setWillReturn();
    return f;
}

Value* guest_ptr(IRBuilder<>& b, Module& m, Value* table, Value* addr) {
    auto* type = FunctionType::get(b.getPtrTy(), {b.getPtrTy(), b.getInt32Ty()}, false);
    return b.CreateCall(declare(m, kGuestPtr, type), {table, addr}, "guest_ptr");
}

Value* guest_ptr_in(IRBuilder<>& b, Module& m, Value* table, Value* addr, Value* lo, Value* size) {
    auto* i32 = b.getInt32Ty();
    auto* type = FunctionType::get(b.getPtrTy(), {b.getPtrTy(), i32, i32, i32}, false);
    return b.CreateCall(declare(m, kGuestPtrIn, type), {table, addr, lo, size}, "guest_ptr");
}

Value* guest_aligned(IRBuilder<>& b, Module& m, Value* addr, unsigned width) {
    auto* type = FunctionType::get(b.getInt1Ty(), {b.getInt32Ty(), b.getInt32Ty()}, false);
    return b.CreateCall(declare(m, kGuestAligned, type), {addr, b.getInt32(width)}, "guest_aligned");
}

bool has_guest_memory(const Module& m) {
    return m.getFunction(kGuestPtr) || m.getFunction(kGuestPtrIn) || m.getFunction(kGuestAligned);
}

static Function* callee_of(const Instruction& inst) {
    auto* call = dyn_cast<CallInst>(&inst);
    return call ? call->getCalledFunction() : nullptr;
}

// One call in a loop the guard can prove for every iteration: its address is
// start + step * i in iteration i, start and step invariant in the loop.
namespace {
struct Access {
    CallInst* call;
    const SCEV* start;
    const SCEV* step;
    unsigned width;   // bytes accessed (for bcore.guest.aligned, the alignment)
    bool pointer;     // bcore.guest.ptr; else bcore.guest.aligned
    // bcore.guest.ptr.in: the window [lo, lo + size) every address lies in,
    // both defined before the loop (no start or step then).
    Value* lo = nullptr;
    Value* size = nullptr;
};
}  // namespace

// The widest access through a bcore.guest.ptr result, or 0 where it has a
// user other than a load, a store to it and a compare with null.
static unsigned access_width(CallInst* call, const DataLayout& layout) {
    unsigned width = 0;
    for (auto* user : call->users()) {
        if (auto* load = dyn_cast<LoadInst>(user)) {
            width = std::max(width, unsigned(layout.getTypeStoreSize(load->getType())));
        } else if (auto* store = dyn_cast<StoreInst>(user); store && store->getPointerOperand() == call) {
            width = std::max(width, unsigned(layout.getTypeStoreSize(store->getValueOperand()->getType())));
        } else if (auto* cmp = dyn_cast<ICmpInst>(user); cmp && cmp->isEquality() &&
                   isa<ConstantPointerNull>(cmp->getOperand(1))) {
        } else {
            return 0;
        }
    }
    return width;
}

static bool version(Loop& loop, LoopInfo& li, DominatorTree& dt, ScalarEvolution& se, Function* ptr_fn,
                    Function* in_fn, Function* aligned_fn, std::set<BasicBlock*>& done) {
    BasicBlock* preheader = loop.getLoopPreheader();
    if (!preheader || !loop.hasDedicatedExits() || !loop.isLCSSAForm(dt)) return false;
    const SCEV* count = se.getSymbolicMaxBackedgeTakenCount(&loop);
    if (isa<SCEVCouldNotCompute>(count) || count->getType()->getIntegerBitWidth() > 32) return false;
    const DataLayout& layout = preheader->getModule()->getDataLayout();
    Instruction* at = preheader->getTerminator();

    std::vector<Access> accesses;
    Value* table = nullptr;
    for (auto* block : loop.blocks())
        for (auto& inst : *block) {
            Function* f = callee_of(inst);
            if (!f || (f != ptr_fn && f != aligned_fn && f != in_fn)) continue;
            auto* call = cast<CallInst>(&inst);
            if (f == in_fn) {
                const unsigned width = access_width(call, layout);
                Value* t = call->getArgOperand(0);
                Value* lo = call->getArgOperand(2);
                Value* size = call->getArgOperand(3);
                auto before = [&](Value* v) {
                    auto* def = dyn_cast<Instruction>(v);
                    return !def || dt.dominates(def, at);
                };
                if (!width || (width & (width - 1)) || (table && t != table) || !before(t) || !before(lo) ||
                    !before(size))
                    continue;
                table = t;
                accesses.push_back({call, nullptr, nullptr, width, true, lo, size});
                continue;
            }
            const bool pointer = f == ptr_fn;
            Value* addr = call->getArgOperand(pointer ? 1 : 0);
            unsigned width = 0;
            if (pointer) {
                width = access_width(call, layout);
                // One table, defined before the loop.
                Value* t = call->getArgOperand(0);
                if (table && t != table) continue;
                if (auto* def = dyn_cast<Instruction>(t); def && !dt.dominates(def, at)) continue;
                table = t;
            } else if (auto* w = dyn_cast<ConstantInt>(call->getArgOperand(1))) {
                width = unsigned(w->getZExtValue());
            }
            if (!width || (width & (width - 1))) continue;
            const SCEV* s = se.getSCEV(addr);
            const SCEV *start = nullptr, *step = nullptr;
            if (se.isLoopInvariant(s, &loop)) {
                start = s;
                step = se.getZero(s->getType());
            } else if (auto* rec = dyn_cast<SCEVAddRecExpr>(s); rec && rec->getLoop() == &loop && rec->isAffine()) {
                start = rec->getStart();
                step = rec->getStepRecurrence(se);
                if (!se.isLoopInvariant(start, &loop) || !se.isLoopInvariant(step, &loop)) continue;
            } else {
                continue;
            }
            ScevExpander probe(se, layout, "guest.lver");
            if (!probe.isSafeToExpandAt(start, at) || !probe.isSafeToExpandAt(step, at)) continue;
            accesses.push_back({call, start, step, width, pointer});
        }
    if (accesses.empty()) return false;
    {
        ScevExpander probe(se, layout, "guest.lver");
        if (!probe.isSafeToExpandAt(count, at)) return false;
    }

    // The guard, in the preheader: for each access, every address of every
    // iteration (i from 0 to the count) lies in [lo, hi] within one mapped
    // page, and start and step are multiples of the width.
    IRBuilder<> b(at);
    auto* i64 = b.getInt64Ty();
    ScevExpander expander(se, layout, "guest.lver");
    Value* n = b.CreateZExt(expander.expandCodeFor(count, count->getType(), at), i64, "lver.count");
    Value* ok = b.getTrue();
    std::vector<Value*> pages(accesses.size()), page_vas(accesses.size());
    for (size_t k = 0; k < accesses.size(); ++k) {
        const Access& a = accesses[k];
        if (a.lo) {
            // Its window, [lo, lo + size), with the access's width past it.
            b.SetInsertPoint(at);
            auto* lo = b.CreateZExt(a.lo, i64);
            auto* hi = b.CreateAdd(b.CreateAdd(lo, b.CreateZExt(a.size, i64)), b.getInt64(int64_t(a.width) - 2),
                                   "lver.hi");
            ok = b.CreateAnd(ok, b.CreateICmpNE(a.size, b.getInt32(0)));
            ok = b.CreateAnd(ok, b.CreateICmpULT(hi, b.getInt64(1ull << 32)));
            ok = b.CreateAnd(ok, b.CreateICmpEQ(b.CreateLShr(lo, Memory::kPageBits), b.CreateLShr(hi, Memory::kPageBits)));
            auto* index = b.CreateAnd(b.CreateLShr(lo, Memory::kPageBits), b.getInt64(Memory::kPageTableEntries - 1));
            auto* page = b.CreateLoad(b.getPtrTy(), b.CreateGEP(b.getPtrTy(), table, index), "lver.page");
            ok = b.CreateAnd(ok, b.CreateIsNotNull(page));
            pages[k] = page;
            page_vas[k] = b.CreateTrunc(b.CreateAnd(lo, b.getInt64(~uint64_t(Memory::kPageSize - 1))), b.getInt32Ty(),
                                        "lver.page_va");
            continue;
        }
        Value* start = expander.expandCodeFor(a.start, a.start->getType(), at);
        Value* step = expander.expandCodeFor(a.step, a.step->getType(), at);
        b.SetInsertPoint(at);
        auto* align = b.CreateOr(start, step);
        ok = b.CreateAnd(ok, b.CreateICmpEQ(b.CreateAnd(align, b.getInt32(a.width - 1)), b.getInt32(0)));
        if (!a.pointer) continue;
        auto* first = b.CreateZExt(start, i64);
        auto* last = b.CreateAdd(first, b.CreateMul(b.CreateSExt(step, i64), n));
        auto* lo = b.CreateSelect(b.CreateICmpSLT(first, last), first, last, "lver.lo");
        auto* hi = b.CreateAdd(b.CreateSelect(b.CreateICmpSLT(first, last), last, first), b.getInt64(a.width - 1),
                               "lver.hi");
        ok = b.CreateAnd(ok, b.CreateICmpSGE(lo, b.getInt64(0)));
        ok = b.CreateAnd(ok, b.CreateICmpULT(hi, b.getInt64(1ull << 32)));
        ok = b.CreateAnd(ok, b.CreateICmpEQ(b.CreateLShr(lo, Memory::kPageBits), b.CreateLShr(hi, Memory::kPageBits)));
        // The page's entry, at an index masked to the table so that the load
        // is in it whatever lo is.
        auto* index = b.CreateAnd(b.CreateLShr(lo, Memory::kPageBits), b.getInt64(Memory::kPageTableEntries - 1));
        auto* page = b.CreateLoad(b.getPtrTy(), b.CreateGEP(b.getPtrTy(), table, index), "lver.page");
        ok = b.CreateAnd(ok, b.CreateIsNotNull(page));
        pages[k] = page;
        page_vas[k] = b.CreateTrunc(b.CreateAnd(lo, b.getInt64(~uint64_t(Memory::kPageSize - 1))), b.getInt32Ty(),
                                    "lver.page_va");
    }

    // Each address's offset into its page as an i64 induction of the loop,
    // expanded in the loop before it is copied (the loop itself leaves it
    // unused): base + {start - page,+,step} is a pointer SCEV sees as affine,
    // as the vectorizer needs to see consecutive accesses, which a 32-bit
    // offset extended to 64 bits is not unless SCEV proves it does not wrap.
    std::vector<Value*> offsets(accesses.size());
    for (size_t k = 0; k < accesses.size(); ++k) {
        const Access& a = accesses[k];
        if (!a.pointer || a.lo) continue;
        const SCEV* va = se.getZeroExtendExpr(se.getSCEV(page_vas[k]), i64);
        const SCEV* first = se.getMinusSCEV(se.getZeroExtendExpr(a.start, i64), va);
        const SCEV* offset = se.getAddRecExpr(first, se.getSignExtendExpr(a.step, i64), &loop, SCEV::FlagAnyWrap);
        offsets[k] = expander.expandCodeFor(offset, i64, a.call);
    }

    // The proven copy, entered from the guard; the loop where it fails.
    BasicBlock* guard = preheader;
    BasicBlock* loop_preheader = SplitBlock(guard, guard->getTerminator(), &dt, &li, nullptr, "guest.lver.ph");
    ValueToValueMapTy vmap;
    SmallVector<BasicBlock*, 32> blocks;
    Loop* copy = cloneLoopWithPreheader(loop_preheader, guard, &loop, vmap, ".proven", &li, &dt, blocks);
    remapInstructionsInBlocks(blocks, vmap);
    // Its exits are the loop's: each exit PHI takes the copy's values too.
    SmallVector<BasicBlock*, 8> exits;
    loop.getUniqueExitBlocks(exits);
    for (auto* exit : exits)
        for (auto& phi : exit->phis())
            for (unsigned i = 0, e = phi.getNumIncomingValues(); i < e; ++i) {
                BasicBlock* from = phi.getIncomingBlock(i);
                if (!loop.contains(from)) continue;
                Value* value = phi.getIncomingValue(i);
                auto mapped = vmap.find(value);
                phi.addIncoming(mapped != vmap.end() ? static_cast<Value*>(mapped->second) : value,
                                cast<BasicBlock>(static_cast<Value*>(vmap[from])));
            }
    guard->getTerminator()->eraseFromParent();
    b.SetInsertPoint(guard);
    b.CreateCondBr(ok, copy->getLoopPreheader(), loop_preheader);

    // In the copy every access is the proven one.
    for (size_t k = 0; k < accesses.size(); ++k) {
        auto* call = cast<CallInst>(vmap[accesses[k].call]);
        if (!accesses[k].pointer) {
            call->replaceAllUsesWith(ConstantInt::getTrue(call->getContext()));
            call->eraseFromParent();
            continue;
        }
        std::vector<ICmpInst*> nulls;
        for (auto* user : call->users())
            if (auto* cmp = dyn_cast<ICmpInst>(user)) nulls.push_back(cmp);
        for (auto* cmp : nulls) {
            cmp->replaceAllUsesWith(ConstantInt::getBool(cmp->getContext(), cmp->getPredicate() == ICmpInst::ICMP_NE));
            cmp->eraseFromParent();
        }
        IRBuilder<> at_call(call);
        Value* offset;
        if (accesses[k].lo) {
            // In the window, so in the page: its offset there.
            offset = at_call.CreateZExt(at_call.CreateSub(call->getArgOperand(1), page_vas[k]), at_call.getInt64Ty());
        } else {
            // The expander may have put an invariant offset before the loop.
            auto mapped = vmap.find(offsets[k]);
            offset = mapped != vmap.end() ? static_cast<Value*>(mapped->second) : offsets[k];
        }
        auto* ptr = at_call.CreateInBoundsGEP(at_call.getInt8Ty(), pages[k], offset, "lver.ptr");
        call->replaceAllUsesWith(ptr);
        call->eraseFromParent();
    }
    done.insert(loop.getHeader());
    done.insert(copy->getHeader());
    return true;
}

void count_in_attribute(Function& f, const char* attr) {
    f.addFnAttr(attr, std::to_string(attribute_count(f, attr) + 1));
}

unsigned attribute_count(const Function& f, const char* attr) {
    if (!f.hasFnAttribute(attr)) return 0;
    unsigned n = 0;
    f.getFnAttribute(attr).getValueAsString().getAsInteger(10, n);
    return n;
}

PreservedAnalyses GuestLoopVersioning::run(Function& f, FunctionAnalysisManager& am) {
    Module& m = *f.getParent();
    Function* ptr_fn = m.getFunction(kGuestPtr);
    Function* in_fn = m.getFunction(kGuestPtrIn);
    Function* aligned_fn = m.getFunction(kGuestAligned);
    if (!ptr_fn && !in_fn && !aligned_fn) return PreservedAnalyses::all();
    std::set<BasicBlock*> done;
    bool changed = false;
    for (int round = 0; round < 16; ++round) {
        auto& li = am.getResult<LoopAnalysis>(f);
        auto& dt = am.getResult<DominatorTreeAnalysis>(f);
        auto& se = am.getResult<ScalarEvolutionAnalysis>(f);
        bool versioned = false;
        for (Loop* loop : li.getLoopsInPreorder()) {
            if (!loop->isInnermost() || done.count(loop->getHeader())) continue;
            done.insert(loop->getHeader());
            if (version(*loop, li, dt, se, ptr_fn, in_fn, aligned_fn, done)) {
                count_in_attribute(f, kLoopsProvenAttr);
                versioned = true;
                break;
            }
        }
        if (!versioned) break;
        changed = true;
        am.invalidate(f, PreservedAnalyses::none());
    }
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

void lower_guest_memory(Module& m) {
    for (const char* name : {kGuestPtr, kGuestPtrIn}) {
        Function* f = m.getFunction(name);
        if (!f) continue;
        std::vector<CallInst*> calls;
        for (auto* user : f->users()) calls.push_back(cast<CallInst>(user));
        for (auto* call : calls) {
            IRBuilder<> b(call);
            Value* table = call->getArgOperand(0);
            Value* addr = call->getArgOperand(1);
            auto* index = b.CreateZExt(b.CreateLShr(addr, Memory::kPageBits), b.getInt64Ty(), "page_index");
            auto* page = b.CreateLoad(b.getPtrTy(), b.CreateGEP(b.getPtrTy(), table, index), "page");
            auto* offset = b.CreateZExt(b.CreateAnd(addr, Memory::kPageSize - 1), b.getInt64Ty(), "page_offset");
            auto* ptr = b.CreateGEP(b.getInt8Ty(), page, offset);
            // A compare with null asks whether the page is there; the rest
            // use the address, on the path where it is.
            std::vector<ICmpInst*> nulls;
            bool other = false;
            for (auto* user : call->users()) {
                if (auto* cmp = dyn_cast<ICmpInst>(user); cmp && isa<ConstantPointerNull>(cmp->getOperand(1)))
                    nulls.push_back(cmp);
                else if (!isa<LoadInst>(user) && !isa<StoreInst>(user))
                    other = true;
            }
            if (other) {
                call->replaceAllUsesWith(b.CreateSelect(b.CreateIsNull(page), ConstantPointerNull::get(b.getPtrTy()), ptr));
            } else {
                for (auto* cmp : nulls) cmp->setOperand(0, page);
                call->replaceAllUsesWith(ptr);
            }
            call->eraseFromParent();
        }
        f->eraseFromParent();
    }
    if (Function* f = m.getFunction(kGuestAligned)) {
        std::vector<CallInst*> calls;
        for (auto* user : f->users()) calls.push_back(cast<CallInst>(user));
        for (auto* call : calls) {
            IRBuilder<> b(call);
            auto* width = cast<ConstantInt>(call->getArgOperand(1));
            auto* mask = b.getInt32(uint32_t(width->getZExtValue()) - 1);
            call->replaceAllUsesWith(b.CreateICmpEQ(b.CreateAnd(call->getArgOperand(0), mask), b.getInt32(0)));
            call->eraseFromParent();
        }
        f->eraseFromParent();
    }
}

}  // namespace bcore
