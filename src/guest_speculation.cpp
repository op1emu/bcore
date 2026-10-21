#include "guest_speculation.h"
#include "guest_memory.h"

#include <map>
#include <set>
#include <vector>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/Support/KnownBits.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ScalarEvolutionExpander.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

using namespace llvm;
using namespace llvm::PatternMatch;

namespace {

// The copy runs only where it has this many iterations to do (fewer are not
// worth its entry and commit), and at most this many, so that the sum of
// magnitudes cannot overflow 64 bits.
constexpr uint64_t kMinIterations = 8;
// The copy does not see another thread stop the run (the loop's stop test,
// which it drops): this bounds how much later the run returns then.
constexpr uint64_t kMaxIterations = uint64_t(1) << 16;

struct Accumulator {
    PHINode* phi;
    SelectInst* next;  // select(fits, window(sum), clamp)
    Value* sum;        // phi + x
    Value* x;
    ICmpInst* fits;
};

// s fits in i32: (s + 2^31) <u 2^32, or s == sext(trunc s).
bool is_fits_test(Value* c, Value* s) {
    ICmpInst::Predicate pred;
    const APInt *bias, *bound;
    if (match(c, m_ICmp(pred, m_Add(m_Specific(s), m_APInt(bias)), m_APInt(bound))) &&
        pred == ICmpInst::ICMP_ULT && bias->getBitWidth() == 64 && bias->getZExtValue() == 0x80000000ull &&
        bound->getZExtValue() == 0x100000000ull)
        return true;
    Value* other;
    if (match(c, m_c_ICmp(pred, m_Specific(s), m_Value(other))) && pred == ICmpInst::ICMP_EQ &&
        match(other, m_SExt(m_Trunc(m_Specific(s)))) && cast<Instruction>(other)->getOperand(0)->getType()->isIntegerTy(32))
        return true;
    return false;
}

// t is s within the window: s itself, or s truncated to 32 bits and
// sign-extended back (which is s where s fits).
bool is_window_of(Value* t, Value* s) {
    if (t == s) return true;
    if (match(t, m_AShr(m_Shl(m_Specific(s), m_SpecificInt(32)), m_SpecificInt(32)))) return true;
    Value* inner;
    return match(t, m_SExt(m_Trunc(m_Value(inner)))) && inner == s &&
           cast<Instruction>(t)->getOperand(0)->getType()->isIntegerTy(32);
}

// Whether v is computed, inside the loop, from of.
bool depends_on(Value* v, Value* of, const Loop& loop) {
    SmallPtrSet<Value*, 32> seen;
    SmallVector<Value*, 16> work{v};
    while (!work.empty()) {
        Value* at = work.pop_back_val();
        if (at == of) return true;
        auto* inst = dyn_cast<Instruction>(at);
        if (!inst || !loop.contains(inst) || !seen.insert(at).second) continue;
        for (Value* op : inst->operands()) work.push_back(op);
    }
    return false;
}

// The exit of a block that leaves only when another thread stopped the run:
// it tests an atomic load against 0 and leaves where it is 0 (the chain
// exit's stop test in a loop block).
bool is_stop_test(BasicBlock* exiting, const Loop& loop) {
    auto* br = dyn_cast<BranchInst>(exiting->getTerminator());
    if (!br || !br->isConditional()) return false;
    ICmpInst::Predicate pred;
    Value* value;
    if (!match(br->getCondition(), m_ICmp(pred, m_Value(value), m_Zero()))) return false;
    auto* load = dyn_cast<LoadInst>(value);
    if (!load || !load->isAtomic()) return false;
    const bool exits_if_true = !loop.contains(br->getSuccessor(0));
    if (pred == ICmpInst::ICMP_EQ) return exits_if_true && loop.contains(br->getSuccessor(1));
    if (pred == ICmpInst::ICMP_NE) return !exits_if_true && !loop.contains(br->getSuccessor(1));
    return false;
}

// What happens to one of the loop's header PHIs in the copy.
enum class Kind {
    Accumulator,  // a saturating sum (Accumulator)
    Reduction,    // phi op y, op an associative bit or add operation, y not from phi
    Induction,    // affine in the iteration
    Derived,      // its value at an iteration follows from the others' (derive)
};

struct Classified {
    PHINode* phi;
    Kind kind;
    Instruction* update = nullptr;  // Reduction: phi op y
    Value* y = nullptr;
};

// The copy's values at an iteration of the loop, rebuilt where the copy
// commits from what the copy knows there: the inductions in closed form,
// the sums the copy kept, and everything else as the loop computes it from
// the iteration before, recursively (a load one step back, say).
class Derive {
public:
    Derive(ScalarEvolution& se, const DataLayout& layout, Instruction* at, const Loop& copy, BasicBlock* latch,
           const SCEV* m, std::map<PHINode*, const Classified*> kinds, std::map<PHINode*, PHINode*> original,
           std::map<PHINode*, Value*> finals)
        : se_(se), expander_(se, layout, "spec.derive"), b_(at), at_(at), copy_(copy), latch_(latch), m_(m),
          kinds_(std::move(kinds)), original_(std::move(original)), finals_(std::move(finals)) {}

    // v at iteration m - back (null if it cannot be derived).
    Value* at(Value* v, unsigned back) {
        auto* inst = dyn_cast<Instruction>(v);
        if (!inst || !copy_.contains(inst)) return v;
        // A value from up to 8 iterations back, in at most 4096 instructions.
        if (back > 8 || ++built_ > 4096) return nullptr;
        auto key = std::make_pair(v, back);
        if (auto it = memo_.find(key); it != memo_.end()) return it->second;
        Value* result = nullptr;
        if (auto* phi = dyn_cast<PHINode>(inst); phi && phi->getParent() == copy_.getHeader()) {
            const Classified* k = kinds_.count(phi) ? kinds_[phi] : nullptr;
            if (!k) return nullptr;
            if (k->kind == Kind::Induction) {
                auto* rec = dyn_cast<SCEVAddRecExpr>(se_.getSCEV(original_[phi]));
                if (!rec) return nullptr;
                const SCEV* it = se_.getMinusSCEV(m_, se_.getConstant(m_->getType(), back));
                it = se_.getTruncateOrZeroExtend(it, rec->getType());
                result = expander_.expandCodeFor(rec->evaluateAtIteration(it, se_), phi->getType(), at_);
            } else if (back == 0 && finals_.count(phi)) {
                result = finals_[phi];
            } else if (k->kind == Kind::Derived) {
                result = at(phi->getIncomingValueForBlock(latch_), back + 1);
            }
        } else if (isa<PHINode>(inst)) {
            return nullptr;  // a join inside an iteration
        } else {
            // A sum's update at the iteration before m is the sum at m.
            for (auto& [phi, final] : finals_)
                if (back == 1 && phi->getIncomingValueForBlock(latch_) == v) result = final;
            if (!result) {
                if (inst->mayHaveSideEffects() || isa<CallInst>(inst) && !isa<IntrinsicInst>(inst)) return nullptr;
                Instruction* clone = inst->clone();
                for (unsigned i = 0; i < clone->getNumOperands(); ++i) {
                    Value* op = at(clone->getOperand(i), back);
                    if (!op) { clone->deleteValue(); return nullptr; }
                    clone->setOperand(i, op);
                }
                b_.Insert(clone);
                result = clone;
            }
        }
        memo_[key] = result;
        return result;
    }

private:
    ScalarEvolution& se_;
    SCEVExpander expander_;
    IRBuilder<> b_;
    Instruction* at_;
    const Loop& copy_;
    BasicBlock* latch_;
    const SCEV* m_;
    std::map<PHINode*, const Classified*> kinds_;
    std::map<PHINode*, PHINode*> original_;
    std::map<PHINode*, Value*> finals_;
    std::map<std::pair<Value*, unsigned>, Value*> memo_;
    unsigned built_ = 0;
};

// A copy of the slice computing v inside loop, at `at`: each header PHI
// given by phis (its value for the iteration the slice is for), every other
// instruction of the loop cloned. Null where the slice has a side effect or
// a join.
Value* clone_slice(Value* v, const Loop& loop, const std::map<PHINode*, Value*>& phis, Instruction* at,
                   std::map<Value*, Value*>& memo) {
    auto* inst = dyn_cast<Instruction>(v);
    if (!inst || !loop.contains(inst)) return v;
    if (auto it = memo.find(v); it != memo.end()) return it->second;
    Value* result = nullptr;
    if (auto* phi = dyn_cast<PHINode>(inst)) {
        auto it = phis.find(phi);
        if (it == phis.end()) return nullptr;
        result = it->second;
    } else {
        if (inst->mayHaveSideEffects() || (isa<CallInst>(inst) && !isa<IntrinsicInst>(inst))) return nullptr;
        Instruction* clone = inst->clone();
        for (unsigned i = 0; i < clone->getNumOperands(); ++i) {
            Value* op = clone_slice(clone->getOperand(i), loop, phis, at, memo);
            if (!op) { clone->deleteValue(); return nullptr; }
            clone->setOperand(i, op);
        }
        clone->insertBefore(at);
        result = clone;
    }
    memo[v] = result;
    return result;
}

bool speculate(Loop& loop, LoopInfo& li, DominatorTree& dt, ScalarEvolution& se, std::set<BasicBlock*>& done) {
    BasicBlock* preheader = loop.getLoopPreheader();
    BasicBlock* header = loop.getHeader();
    BasicBlock* latch = loop.getLoopLatch();
    if (!preheader || !latch || !loop.hasDedicatedExits() || !loop.isLCSSAForm(dt)) return false;
    if (!isa<BranchInst>(latch->getTerminator())) return false;

    // m: the iterations the loop surely runs but the last, unless another
    // thread stops the run -- the least of its exits' exact counts.
    SmallVector<BasicBlock*, 8> exiting;
    loop.getExitingBlocks(exiting);
    const SCEV* m = nullptr;
    for (auto* block : exiting) {
        if (!isa<BranchInst>(block->getTerminator())) return false;
        const SCEV* count = se.getExitCount(&loop, block, ScalarEvolution::Exact);
        if (isa<SCEVCouldNotCompute>(count)) {
            if (!is_stop_test(block, loop)) return false;
            continue;
        }
        m = m ? se.getUMinFromMismatchedTypes(m, count) : count;
    }
    if (!m || m->getType()->getIntegerBitWidth() > 64) return false;
    // No stores, no calls but intrinsics that write nothing.
    for (auto* block : loop.blocks())
        for (auto& inst : *block) {
            if (auto* load = dyn_cast<LoadInst>(&inst)) {
                if (load->isVolatile()) return false;
                // An atomic load only for a stop test, which the copy drops.
                if (load->isAtomic())
                    for (auto* user : load->users()) {
                        auto* cmp = dyn_cast<ICmpInst>(user);
                        if (!cmp || !cmp->hasOneUse()) return false;
                        auto* br = dyn_cast<BranchInst>(cmp->user_back());
                        if (!br || !is_stop_test(br->getParent(), loop)) return false;
                    }
                continue;
            }
            if (inst.mayWriteToMemory() || inst.mayThrow()) return false;
            if (isa<CallInst>(inst) && !isa<IntrinsicInst>(inst)) return false;
        }
    const DataLayout& layout = header->getModule()->getDataLayout();
    Instruction* at = preheader->getTerminator();
    {
        SCEVExpander probe(se, layout, "spec");
        if (!probe.isSafeToExpandAt(m, at)) return false;
    }

    // The header PHIs, classified.
    std::vector<Accumulator> accs;
    std::vector<Classified> phis;
    for (auto& phi : header->phis()) {
        Value* next = phi.getIncomingValueForBlock(latch);
        if (phi.getType()->isIntegerTy(64))
            if (auto* select = dyn_cast<SelectInst>(next)) {
                Value* window = select->getTrueValue();
                Value* sum = window;
                if (!match(window, m_AShr(m_Shl(m_Value(sum), m_SpecificInt(32)), m_SpecificInt(32))) &&
                    !match(window, m_SExt(m_Trunc(m_Value(sum)))))
                    sum = window;
                Value* x = nullptr;
                if (is_window_of(window, sum) && match(sum, m_c_Add(m_Specific(&phi), m_Value(x))) &&
                    is_fits_test(select->getCondition(), sum) && ComputeNumSignBits(x, layout) >= 25 &&
                    !depends_on(x, &phi, loop) && dt.dominates(select, latch->getTerminator())) {
                    accs.push_back({&phi, select, sum, x, cast<ICmpInst>(select->getCondition())});
                    phis.push_back({&phi, Kind::Accumulator});
                    continue;
                }
            }
        const SCEV* s = se.getSCEV(&phi);
        if (auto* rec = dyn_cast<SCEVAddRecExpr>(s);
            phi.getType()->isIntegerTy() && rec && rec->getLoop() == &loop && rec->isAffine()) {
            phis.push_back({&phi, Kind::Induction});
            continue;
        }
        auto* update = dyn_cast<BinaryOperator>(next);
        if (update && phi.hasOneUse() && loop.contains(update) &&
            (update->getOpcode() == Instruction::Or || update->getOpcode() == Instruction::Add ||
             update->getOpcode() == Instruction::Xor || update->getOpcode() == Instruction::And) &&
            (update->getOperand(0) == &phi || update->getOperand(1) == &phi) &&
            dt.dominates(update, latch->getTerminator())) {
            Value* y = update->getOperand(update->getOperand(0) == &phi ? 1 : 0);
            if (!depends_on(y, &phi, loop)) {
                phis.push_back({&phi, Kind::Reduction, update, y});
                continue;
            }
        }
        phis.push_back({&phi, Kind::Derived});
    }
    if (accs.empty()) return false;
    for (auto& a : accs)
        for (auto& p : phis)
            if (p.kind != Kind::Induction && p.phi != a.phi && depends_on(a.x, p.phi, loop) &&
                (p.kind == Kind::Accumulator || p.kind == Kind::Reduction))
                return false;

    // Entry: the copy where it has enough iterations.
    IRBuilder<> b(at);
    auto* i64 = b.getInt64Ty();
    auto* i32 = b.getInt32Ty();
    SCEVExpander expander(se, layout, "spec");
    Value* m64 = b.CreateZExtOrTrunc(expander.expandCodeFor(m, m->getType(), at), i64, "spec.m");
    b.SetInsertPoint(at);
    Value* go = b.CreateAnd(b.CreateICmpUGE(m64, b.getInt64(kMinIterations)),
                            b.CreateICmpULE(m64, b.getInt64(kMaxIterations)), "spec.go");
    BasicBlock* guard = preheader;
    BasicBlock* exact_ph = SplitBlock(guard, guard->getTerminator(), &dt, &li, nullptr, "spec.exact.ph");
    ValueToValueMapTy vmap;
    SmallVector<BasicBlock*, 32> blocks;
    Loop* copy = cloneLoopWithPreheader(exact_ph, guard, &loop, vmap, ".spec", &li, &dt, blocks);
    remapInstructionsInBlocks(blocks, vmap);
    BasicBlock* copy_ph = copy->getLoopPreheader();
    BasicBlock* copy_header = copy->getHeader();
    BasicBlock* copy_latch = cast<BasicBlock>(static_cast<Value*>(vmap[latch]));
    guard->getTerminator()->eraseFromParent();
    b.SetInsertPoint(guard);
    b.CreateCondBr(go, copy_ph, exact_ph);
    auto mapped = [&](Value* v) { return static_cast<Value*>(vmap[v]); };
    // The copy leaves to the loop's exits for now: their PHIs take its values
    // too, so the IR is valid whatever happens next.
    SmallVector<BasicBlock*, 8> exits;
    loop.getUniqueExitBlocks(exits);
    for (auto* exit : exits)
        for (auto& phi : exit->phis())
            for (unsigned k = 0, e = phi.getNumIncomingValues(); k < e; ++k) {
                BasicBlock* from = phi.getIncomingBlock(k);
                if (!loop.contains(from)) continue;
                Value* value = phi.getIncomingValue(k);
                auto it = vmap.find(value);
                phi.addIncoming(it != vmap.end() ? static_cast<Value*>(it->second) : value,
                                cast<BasicBlock>(mapped(from)));
            }
    // Where the copy turns out not to work: the loop runs as it did, and
    // what is left of the copy is unreachable, for the cleanup to delete.
    // The function changed all the same.
    auto abandon = [&] {
        guard->getTerminator()->eraseFromParent();
        BranchInst::Create(exact_ph, guard);
        return true;
    };

    // In the copy no step clamps.
    for (auto& a : accs) {
        auto* fits = cast<Instruction>(mapped(a.fits));
        fits->replaceAllUsesWith(ConstantInt::getTrue(header->getContext()));
        mapped(a.next)->replaceAllUsesWith(mapped(a.sum));
    }

    // Bits of a register a step never changes are the entry's (a load into
    // its low half keeps its high half): then nothing in the copy depends on
    // the register's value from an iteration before, but on what it loads.
    for (auto& phi : copy_header->phis()) {
        if (!phi.getType()->isIntegerTy() || phi.getNumIncomingValues() != 2) continue;
        Value* next = phi.getIncomingValueForBlock(copy_latch);
        Value *kept = nullptr, *rest = nullptr;
        const APInt* mask;
        if (!match(next, m_c_Or(m_Value(kept), m_Value(rest))) || !match(kept, m_And(m_Specific(&phi), m_APInt(mask))))
            continue;
        if (!(computeKnownBits(rest, layout).Zero & *mask).eq(*mask)) continue;
        auto* entry = BinaryOperator::CreateAnd(phi.getIncomingValueForBlock(copy_ph),
                                                ConstantInt::get(phi.getType(), *mask), "spec.kept",
                                                copy_ph->getTerminator());
        cast<Instruction>(kept)->replaceAllUsesWith(entry);
    }

    // The copy runs m - 1 iterations: in iteration j, each sum adds the
    // term of iteration j + 1, from the values iteration j leaves for the
    // next (a load's), so that no term needs a value from a previous
    // iteration; the term of iteration 0 comes before it. Sums in 32 bits:
    // within the window the low 32 bits make the sum.
    auto& ctx = header->getContext();
    auto* commit = BasicBlock::Create(ctx, "spec.commit", header->getParent());
    std::map<PHINode*, Value*> next_values, entry_values;
    for (auto& phi : copy_header->phis()) {
        next_values[&phi] = phi.getIncomingValueForBlock(copy_latch);
        entry_values[&phi] = phi.getIncomingValueForBlock(copy_ph);
    }
    std::map<Value*, Value*> memo_next, memo_entry;
    Instruction* latch_at = copy_latch->getTerminator();
    Instruction* ph_at = copy_ph->getTerminator();
    auto* i = PHINode::Create(i64, 2, "spec.i", &copy_header->front());
    struct Sum { PHINode* phi; Value* next; Value* init; };
    std::vector<Sum> sums;  // per accumulator: the sum, then the magnitudes' maximum
    std::map<PHINode*, PHINode*> reductions;  // copy's header PHI -> its rotated sum
    for (auto& a : accs) {
        Value* term = clone_slice(mapped(a.x), *copy, next_values, latch_at, memo_next);
        Value* first = clone_slice(mapped(a.x), *copy, entry_values, ph_at, memo_entry);
        if (!term || !first) return abandon();
        IRBuilder<> at_latch(latch_at), at_ph(ph_at);
        auto* term32 = at_latch.CreateTrunc(term, i32);
        auto* first32 = at_ph.CreateTrunc(first, i32);
        auto* entry32 = at_ph.CreateTrunc(a.phi->getIncomingValueForBlock(exact_ph), i32);
        auto* sum = PHINode::Create(i32, 2, "spec.sum", &copy_header->front());
        auto* sum_next = at_latch.CreateAdd(sum, term32, "spec.sum.next");
        auto* sum_init = at_ph.CreateAdd(entry32, first32);
        sum->addIncoming(sum_init, copy_ph);
        sum->addIncoming(sum_next, copy_latch);
        sums.push_back({sum, sum_next, sum_init});
        // The largest magnitude: in 32 bits where every term fits in 32
        // (|INT32_MIN| is 2^31 unsigned), of the term itself otherwise (one
        // that does not fit makes the bound fail).
        const bool narrow = ComputeNumSignBits(a.x, layout) >= 33;
        auto* mag_ty = narrow ? i32 : i64;
        auto* mag = PHINode::Create(mag_ty, 2, "spec.mag", &copy_header->front());
        auto* mag_next = at_latch.CreateBinaryIntrinsic(
            Intrinsic::umax, mag,
            at_latch.CreateBinaryIntrinsic(Intrinsic::abs, narrow ? term32 : term, at_latch.getFalse()));
        auto* mag_init = at_ph.CreateBinaryIntrinsic(Intrinsic::abs, narrow ? first32 : first, at_ph.getFalse());
        mag->addIncoming(mag_init, copy_ph);
        mag->addIncoming(mag_next, copy_latch);
        sums.push_back({mag, mag_next, mag_init});
    }
    for (auto& p : phis) {
        if (p.kind != Kind::Reduction) continue;
        Value* term = clone_slice(mapped(p.y), *copy, next_values, latch_at, memo_next);
        Value* first = clone_slice(mapped(p.y), *copy, entry_values, ph_at, memo_entry);
        if (!term || !first) return abandon();
        auto* op = cast<BinaryOperator>(mapped(p.update));
        auto* sum = PHINode::Create(p.phi->getType(), 2, "spec.red", &copy_header->front());
        auto* sum_next = BinaryOperator::Create(op->getOpcode(), sum, term, "spec.red.next", latch_at);
        auto* sum_init = BinaryOperator::Create(op->getOpcode(), p.phi->getIncomingValueForBlock(exact_ph), first,
                                                "", ph_at);
        sum->addIncoming(sum_init, copy_ph);
        sum->addIncoming(sum_next, copy_latch);
        reductions[cast<PHINode>(mapped(p.phi))] = sum;
        sums.push_back({sum, sum_next, sum_init});
    }
    // The copy's exits go; it counts m - 1 iterations.
    for (auto* block : blocks) {
        auto* br = dyn_cast<BranchInst>(block->getTerminator());
        if (!br || !br->isConditional() || block == copy_latch) continue;
        const bool in0 = copy->contains(br->getSuccessor(0)), in1 = copy->contains(br->getSuccessor(1));
        if (in0 == in1) continue;
        auto* cond = dyn_cast<Instruction>(br->getCondition());
        br->getSuccessor(in0 ? 1 : 0)->removePredecessor(block);
        BranchInst::Create(br->getSuccessor(in0 ? 0 : 1), br);
        br->eraseFromParent();
        // A stop test's compare and atomic load go with it: the vectorizer
        // takes no atomic access.
        if (cond && cond->use_empty()) {
            auto* load = dyn_cast<LoadInst>(cond->getOperand(0));
            cond->eraseFromParent();
            if (load && load->isAtomic() && load->use_empty()) load->eraseFromParent();
        }
    }
    {
        IRBuilder<> at_latch(latch_at);
        auto* i_next = at_latch.CreateAdd(i, at_latch.getInt64(1), "spec.i.next");
        auto* more = at_latch.CreateICmpULT(i_next, at_latch.CreateSub(m64, at_latch.getInt64(1)), "spec.more");
        auto* br = at_latch.CreateCondBr(more, copy_header, commit);
        for (auto* succ : successors(copy_latch))
            if (succ != copy_header && succ != commit) succ->removePredecessor(copy_latch);
        latch_at->eraseFromParent();
        i->addIncoming(at_latch.getInt64(0), copy_ph);
        i->addIncoming(i_next, copy_latch);
        MDNode* enable = MDNode::get(ctx, {MDString::get(ctx, "llvm.loop.vectorize.enable"),
                                           ConstantAsMetadata::get(ConstantInt::getTrue(ctx))});
        MDNode* interleave = MDNode::get(ctx, {MDString::get(ctx, "llvm.loop.interleave.count"),
                                               ConstantAsMetadata::get(at_latch.getInt32(1))});
        // Not unrolled after: m is the loop's count (63 for the firmware's
        // FIR filler), a few vector iterations, and the runtime unroll's
        // remainder loop cost more than it saved (3% of the FIR benchmark).
        MDNode* nounroll = MDNode::get(ctx, {MDString::get(ctx, "llvm.loop.unroll.disable")});
        MDNode* id = MDNode::getDistinct(ctx, {nullptr, enable, interleave, nounroll});
        id->replaceOperandWith(0, id);
        br->setMetadata(LLVMContext::MD_loop, id);
    }

    // The commit: the copy's sums, through LCSSA PHIs, and the bound.
    IRBuilder<> at_commit(commit);
    auto out = [&](Sum& s) {
        auto* phi = at_commit.CreatePHI(s.next->getType(), 1, "spec.out");
        phi->addIncoming(s.next, copy_latch);
        return phi;
    };
    std::map<PHINode*, Value*> finals;  // the copy's header PHIs at iteration m
    Value* ok = at_commit.getTrue();
    for (size_t k = 0; k < accs.size(); ++k) {
        Value* sum = out(sums[2 * k]);
        Value* mag = out(sums[2 * k + 1]);
        Value* entry = accs[k].phi->getIncomingValueForBlock(exact_ph);
        auto* bound = at_commit.CreateAdd(at_commit.CreateBinaryIntrinsic(Intrinsic::abs, entry, at_commit.getFalse()),
                                          at_commit.CreateMul(m64, mag->getType() == i32 ? at_commit.CreateZExt(mag, i64)
                                              : at_commit.CreateBinaryIntrinsic(Intrinsic::umin, mag,
                                                                                at_commit.getInt64(1ull << 32))));
        ok = at_commit.CreateAnd(ok, at_commit.CreateICmpULE(bound, at_commit.getInt64(0x7fffffff)), "spec.ok");
        finals[cast<PHINode>(mapped(accs[k].phi))] = at_commit.CreateSExt(sum, i64, "spec.acc");
    }
    for (size_t k = 2 * accs.size(); k < sums.size(); ++k)
        for (auto& [copy_phi, sum] : reductions)
            if (sum == sums[k].phi) finals[copy_phi] = out(sums[k]);
    std::map<PHINode*, const Classified*> kinds;
    std::map<PHINode*, PHINode*> original;
    for (auto& p : phis) {
        kinds[cast<PHINode>(mapped(p.phi))] = &p;
        original[cast<PHINode>(mapped(p.phi))] = p.phi;
    }
    auto* commit_end = at_commit.CreateUnreachable();  // where derive builds, until the branch replaces it
    Derive derive(se, layout, commit_end, *copy, copy_latch, m, kinds, original, finals);
    std::vector<std::pair<PHINode*, Value*>> resumed;
    for (auto& p : phis) {
        Value* value = derive.at(mapped(p.phi), 0);
        if (!value) return abandon();
        resumed.push_back({p.phi, value});
    }
    auto* resume = BasicBlock::Create(ctx, "spec.resume", header->getParent(), header);
    BranchInst::Create(resume, exact_ph, ok, commit_end);
    commit_end->eraseFromParent();
    exact_ph->getTerminator()->eraseFromParent();
    BranchInst::Create(resume, exact_ph);
    IRBuilder<> at_resume(resume);
    for (auto& [phi, value] : resumed) {
        auto* merged = at_resume.CreatePHI(phi->getType(), 2, phi->getName() + ".resume");
        merged->addIncoming(phi->getIncomingValueForBlock(exact_ph), exact_ph);
        merged->addIncoming(value, commit);
        phi->setIncomingBlock(phi->getBasicBlockIndex(exact_ph), resume);
        phi->setIncomingValue(phi->getBasicBlockIndex(resume), merged);
    }
    at_resume.CreateBr(header);
    count_in_attribute(*header->getParent(), kLoopsSpeculatedAttr);
    done.insert(header);
    done.insert(copy_header);
    return true;
}

}  // namespace

PreservedAnalyses SatReductionSpeculation::run(Function& f, FunctionAnalysisManager& am) {
    std::set<BasicBlock*> done;
    bool changed = false;
    for (int round = 0; round < 16; ++round) {
        auto& li = am.getResult<LoopAnalysis>(f);
        auto& dt = am.getResult<DominatorTreeAnalysis>(f);
        auto& se = am.getResult<ScalarEvolutionAnalysis>(f);
        bool any = false;
        for (Loop* loop : li.getLoopsInPreorder()) {
            if (!loop->isInnermost() || done.count(loop->getHeader())) continue;
            done.insert(loop->getHeader());
            if (speculate(*loop, li, dt, se, done)) {
                any = true;
                break;
            }
        }
        if (!any) break;
        changed = true;
        am.invalidate(f, PreservedAnalyses::none());
    }
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
