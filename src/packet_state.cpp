#include "packet_state.h"

#include <cstddef>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

#include "cpu_state.h"

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>

using namespace llvm;

namespace {

enum Field { kPc, kInsnLen, kPackets, kFields };
constexpr size_t kOffset[kFields] = {offsetof(CpuState, pc), offsetof(CpuState, insn_len),
                                     offsetof(CpuState, packets)};

std::optional<Field> field_at(Value* ptr, Value* cpu) {
    auto offset = cpu_field_offset(ptr, cpu);
    if (!offset) return std::nullopt;
    for (int f = 0; f < kFields; ++f)
        if (*offset == kOffset[f]) return Field(f);
    return std::nullopt;
}

// What a call does to pc.
enum class PcEffect {
    None,     // never writes it
    Guarded,  // writes it only together with did_jump
    Hwloop,   // Guarded, and writes the fall-through PC when it does not jump
    Any,      // may write it whatever happens
};

PcEffect pc_effect(CallInst& call) {
    auto* callee = call.getCalledFunction();
    if (!callee) return PcEffect::Any;
    const StringRef name = callee->getName();
    if (name == "bfin_putchar" || name == "cec_is_user_mode") return PcEffect::None;
    if (name == "bfin_hwloop_step") {
        // The form after an instruction that jumped passes its own PC twice:
        // did_jump is set, so the step leaves pc alone.
        auto* at = dyn_cast<ConstantInt>(call.getArgOperand(1));
        auto* next = dyn_cast<ConstantInt>(call.getArgOperand(2));
        if (at && next && at->getZExtValue() == next->getZExtValue()) return PcEffect::None;
        return next ? PcEffect::Hwloop : PcEffect::Any;
    }
    // The memory callbacks and the CEC's helpers change pc only by raising
    // an exception or taking an interrupt, which also sets did_jump.
    if (name.startswith("mem_read") || name.startswith("mem_write") || name == "user_mmr_check" ||
        name == "cec_check_pending" || name == "cec_check_sup" || name == "cec_cli" ||
        name == "cec_sti" || name == "cec_push_reti" || name == "cec_pop_reti" || name == "cec_raise" ||
        name == "cec_exception" || name == "cec_data_alignment")
        return PcEffect::Guarded;
    // cec_return_* write the return address without did_jump; bfin_syscall
    // and anything else may write anything.
    return PcEffect::Any;
}

bool reads_state(Instruction& inst) {
    if (isa<ReturnInst>(inst)) return true;
    auto* call = dyn_cast<CallInst>(&inst);
    return call && !isa<IntrinsicInst>(call);
}

// A definition of one field: a store, a helper's write, or the value the
// field had when the block was entered.
struct Def {
    enum Kind { Entry, Store, Known, Unresolved, Opaque } kind;
    Instruction* inst = nullptr;     // the store or call
    ConstantInt* value = nullptr;    // Store with a constant, Known
    int resolved = -1;               // Unresolved from a Hwloop call: its Known def
};

class StateSink {
public:
    using Set = BitVector;
    StateSink(Function& fn, Value* cpu) : fn_(fn), cpu_(cpu) {}
    void run();

private:
    struct Reader {
        Instruction* inst;
        Set reaching;
    };

    void enumerate();
    // Applies inst to the reaching definitions in `in`; with `record`, notes
    // what each reader sees.
    void transfer(BasicBlock& block, std::vector<Set>& state, bool record);
    std::vector<Set> edge(BasicBlock& from, BasicBlock& to, const std::vector<Set>& out);
    bool is_did_jump_exit(BasicBlock& block);
    std::optional<std::pair<BasicBlock*, BasicBlock*>> did_jump_test(BasicBlock& block);
    void rewrite();

    Function& fn_;
    Value* cpu_;
    std::vector<Def> defs_[kFields];
    DenseMap<Instruction*, int> def_of_[kFields];
    // For each Unresolved def of a Guarded call: what reached the call.
    DenseMap<int, Set> before_call_;
    std::vector<Reader> readers_[kFields];
    DenseMap<BasicBlock*, std::vector<Set>> in_, out_;
};

void StateSink::enumerate() {
    for (int f = 0; f < kFields; ++f) {
        defs_[f].push_back({Def::Entry});
        defs_[f].push_back({Def::Opaque});
    }
    for (auto& block : fn_)
        for (auto& inst : block) {
            if (auto* store = dyn_cast<StoreInst>(&inst)) {
                if (auto f = field_at(store->getPointerOperand(), cpu_)) {
                    def_of_[*f][store] = int(defs_[*f].size());
                    defs_[*f].push_back(
                        {Def::Store, store, dyn_cast<ConstantInt>(store->getValueOperand())});
                }
                continue;
            }
            auto* call = dyn_cast<CallInst>(&inst);
            if (!call || isa<IntrinsicInst>(call)) continue;
            const PcEffect effect = pc_effect(*call);
            if (effect == PcEffect::None) continue;
            if (effect == PcEffect::Any) {
                def_of_[kPc][call] = int(defs_[kPc].size());
                defs_[kPc].push_back({Def::Opaque, call});
                continue;
            }
            Def unresolved{Def::Unresolved, call};
            if (effect == PcEffect::Hwloop) {
                unresolved.resolved = int(defs_[kPc].size());
                defs_[kPc].push_back({Def::Known, call, cast<ConstantInt>(call->getArgOperand(2))});
            }
            def_of_[kPc][call] = int(defs_[kPc].size());
            defs_[kPc].push_back(unresolved);
        }
}

// The did_jump test the lifter puts after a helper call
// (LiftVisitor::emit_did_jump_exit): taken and not-taken successors.
std::optional<std::pair<BasicBlock*, BasicBlock*>> StateSink::did_jump_test(BasicBlock& block) {
    auto* br = dyn_cast<BranchInst>(block.getTerminator());
    if (!br || !br->isConditional()) return std::nullopt;
    auto* cmp = dyn_cast<ICmpInst>(br->getCondition());
    if (!cmp || cmp->getPredicate() != ICmpInst::ICMP_NE) return std::nullopt;
    auto* zero = dyn_cast<ConstantInt>(cmp->getOperand(1));
    auto* load = dyn_cast<LoadInst>(cmp->getOperand(0));
    if (!zero || !zero->isZero() || !load) return std::nullopt;
    auto offset = cpu_field_offset(load->getPointerOperand(), cpu_);
    if (!offset || *offset != offsetof(CpuState, did_jump)) return std::nullopt;
    return std::make_pair(br->getSuccessor(0), br->getSuccessor(1));
}

// A return reached only when did_jump is set. Whatever set it wrote pc too
// (a helper), or stored it before setting did_jump (the lifter's jumps, which
// count as a read of pc here), so the return needs nothing from this pass.
bool StateSink::is_did_jump_exit(BasicBlock& block) {
    if (block.size() != 1 || !isa<ReturnInst>(block.front()) || pred_empty(&block)) return false;
    for (auto* pred : predecessors(&block)) {
        auto test = did_jump_test(*pred);
        if (!test || test->first != &block || test->second == &block) return false;
    }
    return true;
}

void StateSink::transfer(BasicBlock& block, std::vector<Set>& state, bool record) {
    const bool dj_exit = is_did_jump_exit(block);
    for (auto& inst : block) {
        if (auto* store = dyn_cast<StoreInst>(&inst)) {
            auto offset = cpu_field_offset(store->getPointerOperand(), cpu_);
            // Setting did_jump promises pc is in memory.
            if (offset && *offset == offsetof(CpuState, did_jump) && record)
                readers_[kPc].push_back({&inst, state[kPc]});
            if (auto f = field_at(store->getPointerOperand(), cpu_)) {
                state[*f].reset();
                state[*f].set(def_of_[*f][store]);
            }
            continue;
        }
        if (auto* load = dyn_cast<LoadInst>(&inst)) {
            if (auto f = field_at(load->getPointerOperand(), cpu_); f && record)
                readers_[*f].push_back({&inst, state[*f]});
            continue;
        }
        if (!reads_state(inst)) continue;
        if (record)
            for (int f = 0; f < kFields; ++f)
                if (f != kPc || !dj_exit) readers_[f].push_back({&inst, state[f]});
        auto it = def_of_[kPc].find(&inst);
        if (it == def_of_[kPc].end()) continue;
        const int id = it->second;
        if (defs_[kPc][id].kind == Def::Unresolved) before_call_[id] = state[kPc];
        state[kPc].reset();
        state[kPc].set(id);
    }
}

// What reaches `to` along the edge from `from`. On the not-taken side of a
// did_jump test, a Guarded helper did not write pc: what reached it holds
// again (for bfin_hwloop_step, its fall-through PC).
std::vector<StateSink::Set> StateSink::edge(BasicBlock& from, BasicBlock& to, const std::vector<Set>& out) {
    auto test = did_jump_test(from);
    if (!test || test->second != &to || test->first == &to) return out;
    std::vector<Set> result = out;
    Set& pc = result[kPc];
    // Resolving can expose another Unresolved def (two helpers before one
    // test); repeat until none is left.
    for (bool changed = true; changed;) {
        changed = false;
        for (int id = pc.find_first(); id != -1; id = pc.find_next(id)) {
            const Def& def = defs_[kPc][id];
            if (def.kind != Def::Unresolved) continue;
            pc.reset(id);
            if (def.resolved >= 0) {
                pc.set(def.resolved);
            } else {
                auto before = before_call_.find(id);
                // Not seen yet in this iteration: an empty set, the fixpoint
                // brings it in.
                if (before != before_call_.end()) pc |= before->second;
            }
            changed = true;
        }
    }
    return result;
}

void StateSink::run() {
    enumerate();
    std::vector<BasicBlock*> order;
    for (auto* block : ReversePostOrderTraversal<Function*>(&fn_)) order.push_back(block);
    auto empty = [&] {
        std::vector<Set> state(kFields);
        for (int f = 0; f < kFields; ++f) state[f].resize(defs_[f].size());
        return state;
    };
    for (auto* block : order) {
        in_[block] = empty();
        out_[block] = empty();
    }
    auto& entry = fn_.getEntryBlock();
    for (bool changed = true; changed;) {
        changed = false;
        for (auto* block : order) {
            std::vector<Set> in = empty();
            if (block == &entry)
                for (int f = 0; f < kFields; ++f) in[f].set(0);  // Entry
            for (auto* pred : predecessors(block)) {
                if (!out_.count(pred)) continue;  // unreachable
                auto from = edge(*pred, *block, out_[pred]);
                for (int f = 0; f < kFields; ++f) in[f] |= from[f];
            }
            in_[block] = in;
            transfer(*block, in, false);
            if (in != out_[block]) {
                out_[block] = std::move(in);
                changed = true;
            }
        }
    }
    for (auto* block : order) {
        std::vector<Set> state = in_[block];
        transfer(*block, state, true);
    }
    rewrite();
}

void StateSink::rewrite() {
    for (int f = 0; f < kFields; ++f) {
        auto& defs = defs_[f];
        // The one constant every definition reaching a reader stores, if any.
        auto agreed = [&](const Set& reaching) -> ConstantInt* {
            ConstantInt* value = nullptr;
            for (int id : reaching.set_bits()) {
                const Def& def = defs[id];
                if ((def.kind != Def::Store && def.kind != Def::Known) || !def.value) return nullptr;
                if (value && value->getValue() != def.value->getValue()) return nullptr;
                value = def.value;
            }
            return value;
        };
        std::vector<bool> kept(defs.size(), false);
        for (auto& reader : readers_[f])
            if (!agreed(reader.reaching))
                for (int id : reader.reaching.set_bits()) kept[id] = true;
        std::vector<bool> removed(defs.size(), false);
        for (size_t id = 0; id < defs.size(); ++id)
            removed[id] = defs[id].kind == Def::Store && defs[id].value && !kept[id];
        for (auto& reader : readers_[f]) {
            ConstantInt* value = agreed(reader.reaching);
            if (!value) continue;
            bool needs = false;
            for (int id : reader.reaching.set_bits()) needs |= removed[id];
            if (!needs) continue;
            auto* i8 = Type::getInt8Ty(fn_.getContext());
            auto* i32 = Type::getInt32Ty(fn_.getContext());
            auto* ptr = GetElementPtrInst::Create(i8, cpu_, {ConstantInt::get(i32, kOffset[f])},
                                                  "", reader.inst);
            new StoreInst(value, ptr, reader.inst);
        }
        for (size_t id = 0; id < defs.size(); ++id) {
            if (!removed[id]) continue;
            auto* store = defs[id].inst;
            auto* ptr = dyn_cast<Instruction>(store->getOperand(1));
            store->eraseFromParent();
            if (ptr && ptr->use_empty()) ptr->eraseFromParent();
        }
    }
}

}  // namespace

std::optional<size_t> cpu_field_offset(Value* ptr, Value* cpu) {
    ptr = ptr->stripPointerCasts();
    if (ptr == cpu) return 0;
    auto* gep = dyn_cast<GEPOperator>(ptr);
    if (!gep || gep->getPointerOperand()->stripPointerCasts() != cpu || gep->getNumIndices() != 1 ||
        !gep->getSourceElementType()->isIntegerTy(8))
        return std::nullopt;
    auto* index = dyn_cast<ConstantInt>(gep->getOperand(1));
    if (!index) return std::nullopt;
    return size_t(index->getZExtValue());
}

void sink_packet_state(Function& fn, Value* cpu) {
    StateSink(fn, cpu).run();
}
