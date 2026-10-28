#include "steady_skip.h"
#include "llvm_compat.h"
#include "scev_expander.h"
#include "guest_memory.h"
#include "block_abi.h"

#include "cpu_state.h"

#include <cstddef>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Transforms/Utils/ScalarEvolutionExpander.h>

namespace bcore {

using namespace llvm;

namespace {

// The offset from cpu of a pointer into CpuState, or none. (In the width of
// the target's pointer index: 32 bits on WebAssembly.)
std::optional<int64_t> cpu_offset(const Value* ptr, const Value* cpu, const DataLayout& layout) {
    APInt offset(layout.getIndexTypeSizeInBits(ptr->getType()), 0);
    const Value* base = ptr->stripAndAccumulateConstantOffsets(layout, offset, /*AllowNonInbounds=*/true);
    if (base != cpu) return std::nullopt;
    return offset.getSExtValue();
}

bool is_memory_read(const CallInst& call) {
    const Function* f = call.getCalledFunction();
    return f && starts_with(f->getName(), "mem_read");
}

struct Plan {
    const SCEV* last = nullptr;        // the iteration of the first counted exit
    std::vector<PHINode*> counters;    // affine header PHIs
    std::vector<PHINode*> others;      // the rest: must come back to their values
    bool reads = false;                // memory read callbacks: unstable_reads decides too
};

// Whether the loop qualifies, and what the test needs.
bool plan_loop(Loop& loop, ScalarEvolution& se, DominatorTree& dt, Value* cpu, const DataLayout& layout, Plan& plan) {
    BasicBlock* preheader = loop.getLoopPreheader();
    BasicBlock* header = loop.getHeader();
    BasicBlock* latch = loop.getLoopLatch();
    if (!preheader || !latch || !loop.hasDedicatedExits() || !loop.isLCSSAForm(dt)) return false;
    if (!isa<BranchInst>(latch->getTerminator())) return false;

    // The affine values (A): the counters and what is computed from them.
    std::set<const Instruction*> affine;
    for (auto* block : loop.blocks())
        for (auto& inst : *block) {
            if (!inst.getType()->isIntegerTy() || !se.isSCEVable(inst.getType())) continue;
            auto* rec = dyn_cast<SCEVAddRecExpr>(se.getSCEV(&inst));
            if (rec && rec->getLoop() == &loop && rec->isAffine() && se.isKnownNonZero(rec->getStepRecurrence(se)))
                affine.insert(&inst);
        }
    for (auto& phi : header->phis()) {
        if (affine.count(&phi)) plan.counters.push_back(&phi);
        else if (phi.getType()->isIntegerTy() || phi.getType()->isPointerTy()) plan.others.push_back(&phi);
        else return false;
    }
    if (plan.counters.empty()) return false;
    // A loop that waits recomputes its other values from what it reads, or
    // leaves them alone, or ORs a sticky flag into itself (which comes back
    // the same once it has). One that folds a value into itself otherwise
    // (an accumulator, a running sum) is computing, and an iteration of it
    // is steady only by chance: no test for it, which would cost every
    // iteration a compare per PHI.
    for (PHINode* phi : plan.others) {
        Value* next = phi->getIncomingValueForBlock(latch);
        if (next == phi) continue;
        if (auto* op = dyn_cast<BinaryOperator>(next);
            op && (op->getOpcode() == Instruction::Or || op->getOpcode() == Instruction::And) &&
            (op->getOperand(0) == phi || op->getOperand(1) == phi))
            next = op->getOperand(0) == phi ? op->getOperand(1) : op->getOperand(0);
        std::vector<const Value*> work{next};
        std::set<const Value*> seen;
        while (!work.empty()) {
            const auto* inst = dyn_cast<Instruction>(work.back());
            work.pop_back();
            if (!inst || !loop.contains(inst) || !seen.insert(inst).second) continue;
            if (inst == phi) return false;
            if (isa<PHINode>(inst) && inst->getParent() == header) continue;
            for (const Value* op : inst->operands()) work.push_back(op);
        }
    }

    // The exits: counted ones give the last iteration; the others must not
    // depend on a counter.
    SmallVector<BasicBlock*, 8> exiting;
    loop.getExitingBlocks(exiting);
    std::set<const BasicBlock*> counted;
    for (auto* block : exiting) {
        auto* br = dyn_cast<BranchInst>(block->getTerminator());
        if (!br) return false;
        const SCEV* count = se.getExitCount(&loop, block, ScalarEvolution::Exact);
        if (!isa<SCEVCouldNotCompute>(count)) {
            plan.last = plan.last ? se.getUMinFromMismatchedTypes(plan.last, count) : count;
            counted.insert(block);
            continue;
        }
        if (!br->isConditional()) return false;
        std::vector<const Value*> work{br->getCondition()};
        std::set<const Value*> seen;
        while (!work.empty()) {
            const auto* inst = dyn_cast<Instruction>(work.back());
            work.pop_back();
            if (!inst || !loop.contains(inst) || !seen.insert(inst).second) continue;
            if (affine.count(inst)) return false;
            if (isa<PHINode>(inst) && inst->getParent() == header) continue;  // a value of the iteration
            for (const Value* op : inst->operands()) work.push_back(op);
        }
    }
    if (!plan.last || plan.last->getType()->getIntegerBitWidth() > 64) return false;

    // What the counters' values reach (T): computed from them, they may only
    // be stored to CpuState, decide a counted exit, come back to their own
    // counter or leave through an exit's LCSSA PHI. They choose no address,
    // are passed to no call and decide no other branch: those would make an
    // iteration differ from the next.
    std::set<const Instruction*> derived(affine.begin(), affine.end());
    std::vector<const Instruction*> work(affine.begin(), affine.end());
    while (!work.empty()) {
        const Instruction* inst = work.back();
        work.pop_back();
        for (const User* user : inst->users()) {
            const auto* use = cast<Instruction>(user);
            if (!loop.contains(use) || derived.count(use)) continue;  // an LCSSA PHI, or seen
            // Into a header PHI that is no counter (a flag set from a count):
            // its value could pass the test now and change before the last
            // iteration.
            if (isa<PHINode>(use) && use->getParent() == header) return false;
            if (const auto* store = dyn_cast<StoreInst>(use)) {
                if (store->getValueOperand() == inst && cpu_offset(store->getPointerOperand(), cpu, layout)) continue;
                return false;
            }
            if (const auto* br = dyn_cast<BranchInst>(use)) {
                if (counted.count(br->getParent())) continue;
                return false;
            }
            if (isa<LoadInst>(use) || isa<CallBase>(use) || use->mayHaveSideEffects() || isa<SwitchInst>(use))
                return false;
            derived.insert(use);
            work.push_back(use);
        }
    }

    // No effect but on CpuState, no call but the memory reads.
    // [begin, end) offsets and the value stored (null: a copy).
    std::vector<std::tuple<int64_t, int64_t, const Value*>> stored;
    std::vector<const LoadInst*> cpu_loads;
    for (auto* block : loop.blocks())
        for (auto& inst : *block) {
            if (auto* store = dyn_cast<StoreInst>(&inst)) {
                auto offset = cpu_offset(store->getPointerOperand(), cpu, layout);
                if (!offset || store->isVolatile() || store->isAtomic()) return false;
                stored.push_back({*offset, *offset + int64_t(layout.getTypeStoreSize(store->getValueOperand()->getType())),
                                  store->getValueOperand()});
                continue;
            }
            if (auto* load = dyn_cast<LoadInst>(&inst)) {
                if (load->isVolatile()) return false;
                if (cpu_offset(load->getPointerOperand(), cpu, layout)) cpu_loads.push_back(load);
                continue;
            }
            if (auto* call = dyn_cast<CallInst>(&inst)) {
                if (is_memory_read(*call)) { plan.reads = true; continue; }
                if (auto* mem = dyn_cast<MemCpyInst>(call)) {
                    auto offset = cpu_offset(mem->getRawDest(), cpu, layout);
                    auto* length = dyn_cast<ConstantInt>(mem->getLength());
                    if (!offset || !length || mem->isVolatile()) return false;
                    stored.push_back({*offset, *offset + int64_t(length->getZExtValue()), nullptr});
                    continue;
                }
                if (isa<IntrinsicInst>(call) && !call->mayWriteToMemory()) continue;
                return false;
            }
            if (inst.mayWriteToMemory() || inst.mayThrow()) return false;
        }
    // A field the loop writes and reads back carries a value from one
    // iteration to the next past the PHIs: only one the counters do not
    // reach, which a steady iteration stores the same as the last one did
    // (a copy of did_jump before a callback that may set it, then read).
    for (const LoadInst* load : cpu_loads) {
        const int64_t begin = *cpu_offset(load->getPointerOperand(), cpu, layout);
        const int64_t end = begin + int64_t(layout.getTypeStoreSize(load->getType()));
        for (auto [b, e, value] : stored)
            if (begin < e && b < end && (!value || (isa<Instruction>(value) && derived.count(cast<Instruction>(value)))))
                return false;
    }

    ScevExpander probe(se, layout, "steady");
    if (!probe.isSafeToExpandAt(plan.last, preheader->getTerminator())) return false;
    for (PHINode* phi : plan.counters) {
        auto* rec = cast<SCEVAddRecExpr>(se.getSCEV(phi));
        const SCEV* at = rec->evaluateAtIteration(se.getTruncateOrZeroExtend(plan.last, phi->getType()), se);
        if (!probe.isSafeToExpandAt(at, preheader->getTerminator())) return false;
    }
    return true;
}

void transform(Loop& loop, ScalarEvolution& se, Value* cpu, const DataLayout& layout, const Plan& plan, bool dry) {
    BasicBlock* preheader = loop.getLoopPreheader();
    BasicBlock* header = loop.getHeader();
    BasicBlock* latch = loop.getLoopLatch();
    auto& ctx = header->getContext();
    auto* i8 = Type::getInt8Ty(ctx);
    auto* i64 = Type::getInt64Ty(ctx);

    // Each counter at the last iteration, from its start and step at entry.
    std::vector<Value*> at_last;
    {
        ScevExpander expander(se, layout, "steady");
        for (PHINode* phi : plan.counters) {
            auto* rec = cast<SCEVAddRecExpr>(se.getSCEV(phi));
            const SCEV* at = rec->evaluateAtIteration(se.getTruncateOrZeroExtend(plan.last, phi->getType()), se);
            at_last.push_back(expander.expandCodeFor(at, phi->getType(), preheader->getTerminator()));
        }
    }

    IRBuilder<> b(latch->getTerminator());
    Value* steady = b.getTrue();
    for (PHINode* phi : plan.others)
        steady = b.CreateAnd(steady, b.CreateICmpEQ(phi->getIncomingValueForBlock(latch), phi), "steady");
    auto field = [&](IRBuilder<>& at, size_t offset) {
        return at.CreateGEP(i8, cpu, at.getInt64(offset));
    };
    if (plan.reads) {
        IRBuilder<> at_top(&*header->getFirstInsertionPt());
        auto* before = at_top.CreateLoad(i64, field(at_top, offsetof(CpuState, unstable_reads)), "unstable_before");
        auto* after = b.CreateLoad(i64, field(b, offsetof(CpuState, unstable_reads)), "unstable_after");
        steady = b.CreateAnd(steady, b.CreateICmpEQ(before, after), "steady");
    }
    if (dry) {
        auto* ptr = field(b, offsetof(CpuState, steady_iterations));
        b.CreateStore(b.CreateAdd(b.CreateLoad(i64, ptr), b.CreateZExt(steady, i64)), ptr);
    } else {
        for (size_t i = 0; i < plan.counters.size(); ++i) {
            PHINode* phi = plan.counters[i];
            const int index = phi->getBasicBlockIndex(latch);
            Value* next = phi->getIncomingValue(index);
            phi->setIncomingValue(index, b.CreateSelect(steady, at_last[i], next, "steady.skip"));
        }
    }
    se.forgetLoop(&loop);
    count_in_attribute(*header->getParent(), kLoopsSteadyAttr);
}

}  // namespace

PreservedAnalyses SteadyLoopSkip::run(Function& f, FunctionAnalysisManager& am) {
    if (f.arg_empty()) return PreservedAnalyses::all();
    Value* cpu = bcore_abi::block_cpu(&f);
    auto& li = am.getResult<LoopAnalysis>(f);
    auto& dt = am.getResult<DominatorTreeAnalysis>(f);
    auto& se = am.getResult<ScalarEvolutionAnalysis>(f);
    const DataLayout& layout = f.getParent()->getDataLayout();
    std::vector<Loop*> innermost;
    for (Loop* loop : li.getLoopsInPreorder())
        if (loop->getSubLoops().empty()) innermost.push_back(loop);
    bool changed = false;
    for (Loop* loop : innermost) {
        Plan plan;
        if (!plan_loop(*loop, se, dt, cpu, layout, plan)) continue;
        transform(*loop, se, cpu, layout, plan, dry_);
        changed = true;
    }
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}  // namespace bcore
