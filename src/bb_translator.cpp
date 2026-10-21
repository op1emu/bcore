#include "bb_translator.h"

#include <cstddef>
#include <cstdio>
#include <optional>
#include <bitset>
#include <map>
#include <vector>

#include "chain_table.h"
#include "cpu_state.h"
#include "mem.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Analysis/ValueTracking.h>

#include "decoder.h"
#include "lift_visitor.h"
#include "packet_state.h"

using namespace llvm;

BBTranslator::BBTranslator(Memory* mem,
                           bool unlimited, bool fastmem, uint64_t fast_base)
    : mem_(mem), unlimited_(unlimited), fastmem_(fastmem), fast_base_(fast_base) {}

// A block that may run itself again in a native loop is lifted a second
// time as a loop block (translate_as(..., loop = true)) where the first
// lifting says it qualifies: chaining, unlimited and not dry, not careful,
// its loop bottoms steady and no CYCLES access.
//
// The loop block's shadow holds only what the loop uses: the fields the
// packets from its start to its last back edge reach, by the first lifting.
// The rest of the block reaches the others in CpuState, as a plain block
// does, so values the loop does not touch are not carried across it.
BBTranslateResult BBTranslator::translate(uint32_t pc, const CpuState* hint, bool careful) {
    LoopPlan loop;
    BBTranslateResult plain = translate_as(pc, hint, careful, nullptr, &loop);
    if (loop.fields.empty()) return plain;
    BBTranslateResult looped = translate_as(pc, hint, careful, &loop, nullptr);
    // A shadow the copies cannot follow (expand_loop_syncs): the plain block.
    if (!looped.module) return plain;
    plain.module.reset();  // before its context
    return looped;
}

// loop: what a loop block keeps in its shadow and assumes, or null for a
// plain block; plan, for a plain block: set to what a loop block would, or
// left empty where the block does not qualify.
BBTranslateResult BBTranslator::translate_as(uint32_t pc, const CpuState* hint, bool careful,
                                             const LoopPlan* loop, LoopPlan* plan) {
    auto ctx = std::make_unique<LLVMContext>();
    auto mod = std::make_unique<Module>("bb_module", *ctx);
    // Before the first instruction is built: IRBuilder takes load/store
    // alignment from the layout, and the IR pipeline reads it (legal integer
    // widths, i64 alignment). LLJIT's addIRModule accepts a module whose layout
    // equals its own and rejects any other, so the strings come from that same
    // JIT (Core passes JitEngine::dataLayout/targetTriple).
    if (!data_layout_.empty()) mod->setDataLayout(data_layout_);
    if (!target_triple_.empty()) mod->setTargetTriple(target_triple_);

    // Create function: void bb_0x<pc>(CpuState*, Memory*)
    char fname[32];
    snprintf(fname, sizeof(fname), careful ? "bb_0x%08x.careful" : "bb_0x%08x", pc);

    auto* i8ptr = Type::getInt8PtrTy(*ctx);
    auto* ft = FunctionType::get(Type::getVoidTy(*ctx), {i8ptr, i8ptr}, false);
    auto* fn = Function::Create(ft, Function::ExternalLinkage, fname, mod.get());
    fn->setCallingConv(CallingConv::GHC);  // see BbFunc

    auto* entry = BasicBlock::Create(*ctx, "entry", fn);
    IRBuilder<> builder(entry);

    auto arg_it = fn->arg_begin();
    Value* cpu_ptr = &*arg_it++;
    cpu_ptr->setName("cpu");
    Value* mem_ptr = &*arg_it++;
    mem_ptr->setName("mem");

    // Declare external functions used by the lifter
    auto declare_extern = [&](const char* name, FunctionType* ft) {
        mod->getOrInsertFunction(name, ft);
    };

    auto* void_ty = Type::getVoidTy(*ctx);
    auto* i32_ty = Type::getInt32Ty(*ctx);
    auto* i8_ty = Type::getInt8Ty(*ctx);
    auto* i16_ty = Type::getInt16Ty(*ctx);

    declare_extern("bfin_syscall", FunctionType::get(void_ty, {i8ptr, i8ptr}, false));
    declare_extern("mem_read8", FunctionType::get(i8_ty, {i8ptr, i32_ty}, false));
    declare_extern("mem_read16", FunctionType::get(i16_ty, {i8ptr, i32_ty}, false));
    declare_extern("mem_read32", FunctionType::get(i32_ty, {i8ptr, i32_ty}, false));
    declare_extern("mem_write8", FunctionType::get(void_ty, {i8ptr, i32_ty, i8_ty}, false));
    declare_extern("mem_write16", FunctionType::get(void_ty, {i8ptr, i32_ty, i16_ty}, false));
    declare_extern("mem_write32", FunctionType::get(void_ty, {i8ptr, i32_ty, i32_ty}, false));

    // A loop block's guest state: the shadow the visitor lifts into, copied
    // from the real state at entry (expand_loop_syncs).
    Value* state = cpu_ptr;
    if (loop) {
        auto* shadow = builder.CreateAlloca(ArrayType::get(Type::getInt8Ty(*ctx), LiftVisitor::kShadowSize), nullptr,
                                            "shadow");
        shadow->setAlignment(Align(8));
        state = shadow;
    }
    LiftVisitor visitor(builder, state, mem_ptr, mod.get(),
                        mem_, unlimited_,
                        fastmem_, fast_base_, mem_->rawmem_limit());
    if (loop) visitor.set_loop_mode(cpu_ptr, loop->fields, loop->l_zero);
    visitor.set_hint(hint);
    visitor.set_careful(careful);
    visitor.set_idle_waits(idle_waits_);
    // What the entry guard compares the loop bottoms with: the hint's, as the
    // lifter saw them.
    const uint32_t hint_lb[2] = {hint ? hint->lb[0] : 0, hint ? hint->lb[1] : 0};
    const uint32_t hint_lt[2] = {hint ? hint->lt[0] : 0, hint ? hint->lt[1] : 0};
    uint32_t last_pc = pc;

    uint32_t cur_pc = pc;
    BasicBlock* first_entry = nullptr;  // the first packet's entry
    int max_insns = static_cast<int>(max_packets_); // safety limit per BB
    uint32_t packets = 0;

    while (max_insns-- > 0 && !visitor.is_terminated()) {
        uint32_t insn_pc = cur_pc;
        last_pc = insn_pc;
        auto& ctx = builder.getContext();
        char label_name[32];
        snprintf(label_name, sizeof(label_name), "insn_0x%08x_entry", insn_pc);
        auto* insn_entry = BasicBlock::Create(ctx, label_name, fn);
        if (!first_entry) first_entry = insn_entry;
        snprintf(label_name, sizeof(label_name), "insn_0x%08x", insn_pc);
        auto* insn = BasicBlock::Create(ctx, label_name, fn);
        visitor.set_packet_pc(insn_pc);

        auto* previous = builder.GetInsertBlock();
        builder.CreateBr(insn_entry);
        builder.SetInsertPoint(insn);
        // insn_entry, between the two, writes insn_len and packets only.
        visitor.carry_reg_cache(previous, insn);
        int bytes = decodeInstruction(visitor, cur_pc);  // sets visitor.current_pc = cur_pc
        cur_pc += bytes;
        visitor.set_fallthrough_pc(cur_pc);
        visitor.finalize_pending_exits(insn_pc, bytes);
        // Limited mode (CpuState::steps_remaining != 0 at init) leaves the block
        // once the budget runs out; unlimited mode emits nothing here.
        if (!visitor.is_terminated())
            visitor.emit_step_check(cur_pc);

        auto* block = builder.GetInsertBlock();
        builder.SetInsertPoint(insn_entry);
        visitor.emit_insn_len(bytes);
        visitor.emit_packet_index(++packets);
        builder.CreateBr(insn);
        builder.SetInsertPoint(block);
    }

    // Terminate the BB function.
    if (!visitor.is_terminated()) {
        builder.CreateRetVoid();
    } else {
        auto* block = builder.GetInsertBlock();
        if (!block->getTerminator())
            builder.CreateRetVoid();
    }

    const unsigned l_zero = loop ? loop->l_zero : 0;
    BasicBlock* lb_guard = visitor.lb_guard_used() || l_zero
        ? emit_lb_guard(builder, fn, state, cpu_ptr, visitor, first_entry, pc, last_pc,
                        visitor.lb_guard_used() ? hint_lb : nullptr, hint_lt, visitor.lb_pinned(), l_zero)
        : nullptr;
    // Before the chain exit turns the returns into branches to it. A loop
    // block's packet state is in its shadow, which holds it in SSA values
    // and copies it out before each call and return (expand_loop_syncs).
    if (lazy_state_ && !loop) sink_packet_state(*fn, cpu_ptr);
    if (host_cycles_ && visitor.accesses_cycles()) emit_host_cycles(builder, cpu_ptr, first_entry);
    // A native loop where the block may well branch to itself: a jump to its
    // start, or the start of a hardware loop now, unless the filter rejects
    // the block. Elsewhere a jump to itself chains through the table, as to
    // any block.
    const bool self_loop = (visitor.jumps_to(pc) || (hint && (hint->lt[0] == pc || hint->lt[1] == pc))) &&
                           (!chain_filter_ || chain_filter_(pc));
    // A native loop that may have moved a loop bottom passes the guard again.
    BasicBlock* loop_top = lb_guard && !visitor.lb_steady() ? lb_guard : first_entry;
    if (plan && loop_blocks_ && self_loop && chaining_ && unlimited_ && !chain_dry_ && !careful &&
        !visitor.accesses_cycles()) {
        std::set<size_t>* loop_fields = &plan->fields;
        // The loop: from the start to the last packet that branches back to it.
        uint32_t last = 0;
        for (auto [from, to] : visitor.edges_to())
            if (to == pc) last = std::max(last, from);
        if (hint)
            for (int i = 0; i < 2; ++i)
                if (hint->lt[i] == pc && hint->lb[i] >= pc) last = std::max(last, hint->lb[i]);
        if (last)
            for (auto [offset, first] : visitor.field_first_pc())
                if (first <= last && LiftVisitor::shadowable(offset)) loop_fields->insert(offset);
        // An accumulator's two fields together: the shadow keeps it whole too
        // (LiftVisitor::kAccSlot), following both.
        for (int n = 0; n < 2; ++n)
            if (loop_fields->count(offsetof(CpuState, ax[0]) + 4 * n) || loop_fields->count(offsetof(CpuState, aw[0]) + 4 * n)) {
                loop_fields->insert(offsetof(CpuState, ax[0]) + 4 * n);
                loop_fields->insert(offsetof(CpuState, aw[0]) + 4 * n);
            }
        // Linear DAG registers the block reads but never writes: assumed so.
        if (last && hint)
            for (int i = 0; i < 4; ++i) {
                const size_t l = offsetof(CpuState, lregs[0]) + 4 * i;
                if (hint->lregs[i] == 0 && loop_fields->count(l) && !visitor.written_fields().count(l))
                    plan->l_zero |= 1u << i;
            }
    }
    if (chaining_ && unlimited_)
        emit_chain_exit(builder, fn, state, cpu_ptr, mem_ptr, pc, self_loop ? loop_top : nullptr,
                        lb_guard ? lb_guard_miss_block(lb_guard) : nullptr, first_entry,
                        &visitor.steady_back_edges());
    if (loop) {
        if (!expand_loop_syncs(fn, cast<AllocaInst>(state), cpu_ptr, steady_skip_)) {
            mod.reset();  // before its context
            return {nullptr, std::move(ctx), visitor.fallthrough_pc(), "packet-cap"};
        }
        fn->addFnAttr(kLoopBlockAttr);
    }

    // Verify the module
    std::string err_str;
    raw_string_ostream err_stream(err_str);
    if (verifyModule(*mod, &err_stream)) {
        fprintf(stderr, "Module verification failed for %s:\n%s\n",
                fname, err_str.c_str());
    }

    const char* exit_kind = visitor.is_terminated() ? visitor.exit_kind()
                             : budget_limited_ ? "step-budget" : "packet-cap";
    return {std::move(mod), std::move(ctx), visitor.fallthrough_pc(), exit_kind};
}

static Value* field(IRBuilder<>& b, Value* cpu, size_t offset) {
    return b.CreateGEP(b.getInt8Ty(), cpu, b.getInt64(offset));
}

// A loop block's shadow (BBTranslator::set_loop_blocks), once lifted. The
// shadow starts with the real state's values of every field the block reads
// or writes (touched, from the loads and stores at the shadow's constant
// offsets), so a field is clean -- the same in both -- until the block
// stores to it. Where a helper call (LiftVisitor::kSyncOut) or an exit
// (kCommit) needs the real state current, the block copies the fields dirty
// there, from a forward dataflow over the function: dirty after a store
// to the field on some path since the last copy out. After a call that may
// have changed the real state (kSyncIn) it copies every touched field back,
// and all are clean again. Copying only dirty fields keeps a loop's
// unchanged fields out of its registers: a clean field never needs its
// value carried to a copy.
//
// False, changing nothing, where the shadow is used other than through loads
// and stores at constant offsets: the block is then compiled plain.
bool BBTranslator::expand_loop_syncs(Function* fn, AllocaInst* shadow, Value* real, bool steady_skip) {
    using Bytes = std::bitset<sizeof(CpuState)>;
    const DataLayout& layout = fn->getParent()->getDataLayout();
    Function* sync_out = fn->getParent()->getFunction(LiftVisitor::kSyncOut);
    Function* sync_in = fn->getParent()->getFunction(LiftVisitor::kSyncIn);
    Function* commit = fn->getParent()->getFunction(kCommit);
    auto bytes = [](uint32_t begin, uint32_t size) {
        Bytes set;
        for (uint32_t i = begin; i < begin + size && i < sizeof(CpuState); ++i) set.set(i);
        return set;
    };
    // The bytes each store to the shadow writes, and every byte touched.
    std::map<StoreInst*, Bytes> stores;
    Bytes touched;
    // At ptr, the shadow plus offset (the shadow itself, or one GEP off it):
    // only loads and stores to it.
    auto access = [&](Value* ptr, uint32_t offset, bool base) {
        for (auto* user : ptr->users()) {
            if (auto* load = dyn_cast<LoadInst>(user)) {
                touched |= bytes(offset, uint32_t(layout.getTypeStoreSize(load->getType())));
            } else if (auto* store = dyn_cast<StoreInst>(user); store && store->getPointerOperand() == ptr) {
                const Bytes set = bytes(offset, uint32_t(layout.getTypeStoreSize(store->getValueOperand()->getType())));
                touched |= set;
                stores[store] = set;
            } else if (!(base && isa<GetElementPtrInst>(user))) {
                return false;
            }
        }
        return true;
    };
    if (!access(shadow, 0, true)) return false;
    for (auto* user : shadow->users())
        if (auto* gep = dyn_cast<GetElementPtrInst>(user)) {
            auto offset = cpu_field_offset(gep, shadow);
            if (!offset || *offset >= LiftVisitor::kShadowSize || !access(gep, uint32_t(*offset), false)) return false;
        }

    auto callee = [](Instruction& inst) -> Function* {
        auto* call = dyn_cast<CallInst>(&inst);
        return call ? call->getCalledFunction() : nullptr;
    };
    // What one instruction does to the dirty bytes.
    auto step = [&](Instruction& inst, Bytes& dirty) {
        if (auto* store = dyn_cast<StoreInst>(&inst)) {
            if (auto it = stores.find(store); it != stores.end()) dirty |= it->second;
            return;
        }
        Function* f = callee(inst);
        if (f && (f == sync_out || f == commit || f == sync_in)) dirty.reset();
    };
    std::map<BasicBlock*, Bytes> in;
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& block : *fn) {
            Bytes dirty = in[&block];
            for (auto& inst : block) step(inst, dirty);
            for (auto* succ : successors(&block)) {
                Bytes merged = in[succ] | dirty;
                if (merged != in[succ]) { in[succ] = merged; changed = true; }
            }
        }
    }

    auto& ctx = fn->getContext();
    IRBuilder<> b(ctx);
    auto copy = [&](const Bytes& set, bool to_shadow) {
        for (uint32_t begin = 0; begin < sizeof(CpuState);) {
            if (!set.test(begin)) { ++begin; continue; }
            uint32_t end = begin;
            while (end < sizeof(CpuState) && set.test(end)) ++end;
            auto* at_shadow = b.CreateGEP(b.getInt8Ty(), shadow, b.getInt64(begin));
            auto* at_real = b.CreateGEP(b.getInt8Ty(), real, b.getInt64(begin));
            if (to_shadow) b.CreateMemCpy(at_shadow, Align(1), at_real, Align(1), end - begin);
            else b.CreateMemCpy(at_real, Align(1), at_shadow, Align(1), end - begin);
            begin = end;
        }
    };
    // did_jump is clear when a block starts (Core::run, the chain exit), as
    // the visitor assumes: the shadow starts with it clear rather than
    // copied, so the IR knows it.
    b.SetInsertPoint(fn->getEntryBlock().getTerminator());
    Bytes copied = touched;
    for (uint32_t i = offsetof(CpuState, did_jump); i < offsetof(CpuState, did_jump) + sizeof(bool); ++i)
        copied.reset(i);
    copy(copied, true);
    b.CreateStore(b.getInt8(0), b.CreateGEP(b.getInt8Ty(), shadow, b.getInt64(offsetof(CpuState, did_jump))));

    // A loop that skips to its last iteration (Core::set_steady_skip) jumps
    // the fields it counts in without storing the iterations between: a
    // field is then clean where the real state holds a value from before
    // the skip. Every copy out writes every field the block stores, so the
    // real state never lags behind.
    Bytes stored_anywhere;
    if (steady_skip)
        for (auto& [store, set] : stores) stored_anywhere |= set;
    std::vector<std::pair<CallInst*, Bytes>> syncs;
    for (auto& block : *fn) {
        Bytes dirty = in[&block];
        for (auto& inst : block) {
            Function* f = callee(inst);
            if (f && (f == sync_out || f == commit)) syncs.push_back({cast<CallInst>(&inst), dirty | stored_anywhere});
            else if (f && f == sync_in) syncs.push_back({cast<CallInst>(&inst), touched});
            step(inst, dirty);
        }
    }
    // The whole accumulators (LiftVisitor::kAccSlot) the block uses, built
    // from the real state's fields where the shadow's are copied in.
    bool acc_used[2] = {};
    for (auto* user : shadow->users())
        if (auto* gep = dyn_cast<GetElementPtrInst>(user))
            if (auto offset = cpu_field_offset(gep, shadow); offset && *offset >= LiftVisitor::kAccSlot)
                acc_used[(*offset - LiftVisitor::kAccSlot) / 8] = true;
    auto build_accs = [&] {
        for (int n = 0; n < 2; ++n) {
            if (!acc_used[n]) continue;
            auto* ax = b.CreateLoad(b.getInt32Ty(), b.CreateGEP(b.getInt8Ty(), real, b.getInt64(offsetof(CpuState, ax[0]) + 4 * n)));
            auto* aw = b.CreateLoad(b.getInt32Ty(), b.CreateGEP(b.getInt8Ty(), real, b.getInt64(offsetof(CpuState, aw[0]) + 4 * n)));
            auto* acc = b.CreateOr(b.CreateShl(b.CreateSExt(b.CreateTrunc(ax, b.getInt8Ty()), b.getInt64Ty()), 32),
                                   b.CreateZExt(aw, b.getInt64Ty()));
            b.CreateStore(acc, b.CreateGEP(b.getInt8Ty(), shadow, b.getInt64(LiftVisitor::kAccSlot + 8 * n)));
        }
    };
    b.SetInsertPoint(fn->getEntryBlock().getTerminator());
    build_accs();
    for (auto& [call, set] : syncs) {
        b.SetInsertPoint(call);
        copy(set, call->getCalledFunction() == sync_in);
        if (call->getCalledFunction() == sync_in) build_accs();
        call->eraseFromParent();
    }
    for (Function* decl : {sync_out, sync_in, commit})
        if (decl) decl->eraseFromParent();
    return true;
}

// A host that writes CYCLES before every block (CYCLES2 too, and its shadow)
// sees CYCLES advance by each block's packets. Chained blocks have no host
// between them: one that accesses CYCLES writes it first, to what the host
// would have, at the top of its first packet, so the self loop's back edge
// writes it too. For the run's first block (chain_packets 0) that is the
// value the host wrote.
void BBTranslator::emit_host_cycles(IRBuilder<>& b, Value* cpu, BasicBlock* first_entry) {
    b.SetInsertPoint(first_entry, first_entry->getFirstInsertionPt());
    auto* i32 = b.getInt32Ty();
    auto* i64 = b.getInt64Ty();
    auto* base = b.CreateLoad(i64, field(b, cpu, offsetof(CpuState, cycles_base)), "cycles_base");
    auto* done = b.CreateLoad(i32, field(b, cpu, offsetof(CpuState, chain_packets)), "chain_packets");
    auto* now = b.CreateAdd(base, b.CreateZExt(done, i64), "cycles_now");
    auto* lo = b.CreateTrunc(now, i32);
    auto* hi = b.CreateTrunc(b.CreateLShr(now, 32), i32);
    b.CreateStore(lo, field(b, cpu, offsetof(CpuState, cycles[0])));
    b.CreateStore(hi, field(b, cpu, offsetof(CpuState, cycles[1])));
    b.CreateStore(hi, field(b, cpu, offsetof(CpuState, cycles[2])));
}

// The guard a block needs when packets the hint puts at no loop bottom skip
// the bottom compare (LiftVisitor::lb_absent): at entry, each of LB0 and LB1
// is the hint's or lies outside [first_pc, last_pc], so no such packet is a
// bottom. Otherwise the block sets CpuState::lb_guard_miss and returns to
// Core::run before its first packet, not through the chain exit: pc is its
// own, nothing retired, and the run's chain state is as the block found it,
// so Core::run compiles it again careful and runs that in its place. A loop
// the block pins (LiftVisitor::lb_pinned) needs its bottom and its top to be
// exactly the hint's; hint_lb null for none of this. A loop block's linear
// DAG registers (l_zero, bit i for L[i]) must be. In a loop block (cpu its shadow, real the CpuState)
// the guard reads the shadow, as a native loop that passes it again must,
// and where it misses copies the shadow out first. Returns the guard's block.
BasicBlock* BBTranslator::emit_lb_guard(IRBuilder<>& b, Function* fn, Value* cpu, Value* real,
                                        const LiftVisitor& visitor, BasicBlock* first_entry,
                                        uint32_t first_pc, uint32_t last_pc, const uint32_t hint_lb[2],
                                        const uint32_t hint_lt[2], unsigned pinned, unsigned l_zero) {
    auto& ctx = fn->getContext();
    auto& entry = fn->getEntryBlock();
    auto* guard = BasicBlock::Create(ctx, "lb_guard", fn, first_entry);
    auto* miss = BasicBlock::Create(ctx, "lb_guard_miss", fn);
    auto* br = cast<BranchInst>(entry.getTerminator());
    br->setSuccessor(0, guard);

    b.SetInsertPoint(guard);
    auto* i32 = b.getInt32Ty();
    Value* ok = b.getTrue();
    for (int i = 0; i < 4; ++i)
        if (l_zero >> i & 1) {
            auto* l = b.CreateLoad(i32, field(b, real, offsetof(CpuState, lregs[0]) + 4 * i), "guard_l");
            ok = b.CreateAnd(ok, b.CreateICmpEQ(l, b.getInt32(0)));
        }
    for (int i = 0; hint_lb && i < 2; ++i) {
        auto loop_field = [&](size_t offset) { return field(b, visitor.shadowed(offset) ? cpu : real, offset); };
        auto* lb = b.CreateLoad(i32, loop_field(offsetof(CpuState, lb[0]) + 4 * i), "guard_lb");
        auto* hinted = b.CreateICmpEQ(lb, b.getInt32(hint_lb[i]));
        if (pinned & (1u << i)) {
            auto* lt = b.CreateLoad(i32, loop_field(offsetof(CpuState, lt[0]) + 4 * i), "guard_lt");
            ok = b.CreateAnd(ok, b.CreateAnd(hinted, b.CreateICmpEQ(lt, b.getInt32(hint_lt[i]))));
            continue;
        }
        auto* outside = b.CreateICmpUGT(b.CreateSub(lb, b.getInt32(first_pc)), b.getInt32(last_pc - first_pc));
        ok = b.CreateAnd(ok, b.CreateOr(hinted, outside));
    }
    b.CreateCondBr(ok, first_entry, miss, weights_usually(ctx));

    b.SetInsertPoint(miss);
    b.CreateStore(b.getInt8(1), field(b, real, offsetof(CpuState, lb_guard_miss)));
    if (cpu != real) {
        auto* ft = FunctionType::get(b.getVoidTy(), false);
        b.CreateCall(ft, fn->getParent()->getOrInsertFunction(kCommit, ft).getCallee());
    }
    b.CreateRetVoid();
    return guard;
}

// The guard's other successor: where it misses.
BasicBlock* BBTranslator::lb_guard_miss_block(BasicBlock* guard) {
    return cast<BranchInst>(guard->getTerminator())->getSuccessor(1);
}

// What the last store to cpu+offset before `at` wrote, looking back through
// single predecessors; null when a helper call (which may write the field), a
// store at an offset not known or a merge comes first. Guest memory never
// overlaps CpuState, so other stores are passed over.
static Value* stored_before(Instruction* at, Value* cpu, size_t offset) {
    BasicBlock* block = at->getParent();
    BasicBlock::iterator it(at);
    for (int blocks = 0; blocks < 64; ++blocks) {
        while (it != block->begin()) {
            --it;
            if (auto* store = dyn_cast<StoreInst>(&*it)) {
                Value* ptr = store->getPointerOperand();
                auto at_offset = cpu_field_offset(ptr, cpu);
                if (at_offset && *at_offset == offset) return store->getValueOperand();
                if (!at_offset && getUnderlyingObject(ptr) == cpu) return nullptr;
                continue;
            }
            auto* call = dyn_cast<CallInst>(&*it);
            if (call && !isa<IntrinsicInst>(call)) return nullptr;
        }
        block = block->getSinglePredecessor();
        if (!block) return nullptr;
        it = block->end();
    }
    return nullptr;
}

// The packet state a return's path stores last, in the straight-line tail
// that leads only to it: a chained block needs none of it, so the exit stores
// it only where it returns. kTail* index offsets and values.
enum { kTailPc, kTailInsnLen, kTailPackets, kTailDidJump, kTailFields };
static constexpr size_t kTailOffset[kTailFields] = {offsetof(CpuState, pc), offsetof(CpuState, insn_len),
                                                    offsetof(CpuState, packets), offsetof(CpuState, did_jump)};
struct Tail {
    Value* value[kTailFields] = {};
    bool any() const { return value[0] || value[1] || value[2] || value[3]; }
};

static int tail_field(std::optional<size_t> offset) {
    if (offset)
        for (int f = 0; f < kTailFields; ++f)
            if (*offset == kTailOffset[f]) return f;
    return -1;
}

// Removes the last store to each tail field before ret, if nothing reads it
// before ret: within ret's block and the predecessors before it that have
// no other successor, back to a helper call or a store at an offset not
// known. Returns the values they stored.
static Tail sink_tail(ReturnInst* ret, Value* cpu) {
    StoreInst* found[kTailFields] = {};
    bool read[kTailFields] = {};
    BasicBlock* block = ret->getParent();
    BasicBlock::iterator it(ret);
    for (bool more = true; more;) {
        while (it != block->begin()) {
            --it;
            if (auto* store = dyn_cast<StoreInst>(&*it)) {
                auto offset = cpu_field_offset(store->getPointerOperand(), cpu);
                if (!offset && getUnderlyingObject(store->getPointerOperand()) == cpu) { more = false; break; }
                const int f = tail_field(offset);
                if (f >= 0 && !found[f] && !read[f]) found[f] = store;
                continue;
            }
            if (auto* load = dyn_cast<LoadInst>(&*it)) {
                const int f = tail_field(cpu_field_offset(load->getPointerOperand(), cpu));
                if (f >= 0 && !found[f]) read[f] = true;
                continue;
            }
            auto* call = dyn_cast<CallInst>(&*it);
            if (call && !isa<IntrinsicInst>(call)) { more = false; break; }
        }
        if (!more) break;
        BasicBlock* pred = block->getSinglePredecessor();
        if (!pred || pred->getSingleSuccessor() != block) break;
        block = pred;
        it = block->end();
    }
    Tail tail;
    for (int f = 0; f < kTailFields; ++f) {
        if (!found[f]) continue;
        tail.value[f] = found[f]->getValueOperand();
        auto* ptr = dyn_cast<Instruction>(found[f]->getPointerOperand());
        found[f]->eraseFromParent();
        if (ptr && ptr->use_empty()) ptr->eraseFromParent();
    }
    return tail;
}

// Every return becomes an exit that may run the next block itself. Nearly
// every exit does (the boot to op1emu's main display returns to the host
// about once in 480 block ends), and a native loop runs again: the branches
// say so (weights_usually, weights_loop), and the return goes after the hot
// code. At each return cpu->pc is where the host would continue, so running
// that block here is a host round trip without the host: allowed while the
// run's budget lasts (the CEC, a halt, the host's memory callbacks and other
// threads set it to 0 when the host has work, and so does whatever leaves an
// interrupt pending for the host to check), and the block is compiled and in
// the chain table, which holds no block the host's filter rejects.
//
// Each return gets its own exit, with the pc and packet index its path stored
// (stored_before) rather than loads of them. Where the next PC is a constant
// the exit reads that PC's slot at a constant offset in the table, and where
// it is this block's own PC and self_loop is set (its first packet; set only
// when the filter accepts this PC) a branch there runs it again: a native
// loop, with no table. Other exits share one that branches there too when
// the PC is this block's (a hardware loop's back edge) and otherwise looks
// it up in the table.
//
// cpu is where the block keeps the guest state, real the CpuState: the same
// but in a loop block, whose shadow the exit copies to the real state
// (kCommit, see expand_loop_syncs) before it returns or calls the next block,
// and not where it runs itself again.
//
// A back edge to the block's start in `steady` (the loop registers as the
// entry guard found them) branches past the guard, to first_entry.
void BBTranslator::emit_chain_exit(IRBuilder<>& b, Function* fn, Value* cpu, Value* real, Value* mem,
                                   uint32_t entry_pc, BasicBlock* self_loop, BasicBlock* direct,
                                   BasicBlock* first_entry, const std::set<ReturnInst*>* steady) {
    std::vector<ReturnInst*> returns;
    for (auto& block : *fn)
        if (auto* ret = dyn_cast_or_null<ReturnInst>(block.getTerminator()); ret && &block != direct)
            returns.push_back(ret);
    if (returns.empty()) return;

    auto& ctx = fn->getContext();
    auto* i8 = b.getInt8Ty();
    auto* i32 = b.getInt32Ty();
    auto* ptr = b.getInt8PtrTy();

    // The chain table does not change while a block runs (the host compiles
    // and publishes between runs): loaded once.
    auto& entry = fn->getEntryBlock();
    IRBuilder<> at_entry(&entry, entry.getFirstInsertionPt());
    auto* table = at_entry.CreateLoad(ptr, field(at_entry, real, offsetof(CpuState, chain_table)), "chain_table");
    const bool loop = cpu != real;
    auto commit = [&] {
        if (!loop) return;
        auto* ft = FunctionType::get(b.getVoidTy(), false);
        b.CreateCall(ft, fn->getParent()->getOrInsertFunction(kCommit, ft).getCallee());
    };

    auto* leave = BasicBlock::Create(ctx, "chain_leave", fn);
    b.SetInsertPoint(leave);
    commit();
    b.CreateRetVoid();

    // The budget test every exit makes: on to `go` with the run's packets
    // including this block's, or to `out`. No exit finds an interrupt pending:
    // generated code checks one after every instruction that may enable it,
    // and whatever else makes one pending sets the budget to 0 (cec_raise).
    auto load_budget = [&](IRBuilder<>& at) {
        // Atomic: another thread may set it to 0, and a native loop with no
        // call in it would otherwise read it once.
        auto* budget = at.CreateLoad(i32, field(at, real, offsetof(CpuState, chain_budget)), "chain_budget");
        budget->setAtomic(AtomicOrdering::Monotonic);
        budget->setAlignment(Align(4));
        // Below 2^31 (CpuState::chain_budget), so that the run's packets
        // plus a block's never wrap: ScalarEvolution can then count a loop
        // that retires more than one packet an iteration by its budget.
        budget->setMetadata(LLVMContext::MD_range,
                            MDBuilder(ctx).createRange(APInt(32, 0), APInt(32, 0x80000000u)));
        return budget;
    };
    // A loop block compares with the budget it found at entry, then sees
    // whether the budget is 0 now: the same test, since while a run lasts
    // the budget changes only to 0, but one whose first half counts the
    // loop's iterations (the budget at entry does not change in it), and
    // its second half one load where the plain test had two.
    Value* entry_budget = loop ? load_budget(at_entry) : nullptr;
    auto admit = [&](Value* packets, BasicBlock* go, BasicBlock* out) {
        auto* done = b.CreateLoad(i32, field(b, cpu, offsetof(CpuState, chain_packets)), "chain_packets");
        auto* total = b.CreateNUWAdd(done, packets, "chain_total");
        if (!loop) {
            b.CreateCondBr(b.CreateICmpULT(total, load_budget(b)), go, out, weights_usually(ctx));
            return total;
        }
        auto* now = BasicBlock::Create(ctx, "chain_budget_now", fn);
        b.CreateCondBr(b.CreateICmpULT(total, entry_budget), now, out, weights_usually(ctx));
        b.SetInsertPoint(now);
        b.CreateCondBr(b.CreateICmpNE(load_budget(b), b.getInt32(0)), go, out, weights_usually(ctx));
        return total;
    };
    // The tail stores a return skipped (sink_tail), where the block returns.
    auto store_tail = [&](const Tail& tail) {
        for (int f = 0; f < kTailFields; ++f)
            if (tail.value[f]) b.CreateStore(tail.value[f], field(b, cpu, kTailOffset[f]));
    };
    auto leave_for = [&](const Tail& tail) {
        if (!tail.any()) return leave;
        auto* block = BasicBlock::Create(ctx, "chain_leave", fn);
        IRBuilder<>::InsertPointGuard guard(b);
        b.SetInsertPoint(block);
        store_tail(tail);
        commit();
        b.CreateRetVoid();
        return block;
    };
    // What the host does before the next block: retire, then clear did_jump
    // (Core::run), with pc in place. False in dry mode, where the block
    // returns instead. The packet index stays: a block stores its own before
    // anything reads it, but where its loop-bottom guard misses, and then
    // Core::run clears it before it compiles the block again careful.
    // did_jump_clear: did_jump is clear in memory, as where the tail held the
    // path's store of it (every path's, for the shared exit). A block starts
    // with it clear, and on a path that goes on past a jump or a helper that
    // set it no packet runs (the lifter leaves at once); a path whose helper
    // set it last (bfin_hwloop_step's back edge) has no such store.
    auto chain = [&](Value* total, const Tail& tail, Value* next, bool did_jump_clear) {
        if (chain_dry_) {
            store_tail(tail);
            b.CreateStore(b.getInt8(1), field(b, real, offsetof(CpuState, chain_would)));
            commit();
            b.CreateRetVoid();
            return false;
        }
        if (tail.value[kTailPc]) b.CreateStore(next, field(b, cpu, offsetof(CpuState, pc)));
        b.CreateStore(total, field(b, cpu, offsetof(CpuState, chain_packets)));
        if (!did_jump_clear) b.CreateStore(b.getInt8(0), field(b, cpu, offsetof(CpuState, did_jump)));
        if (count_chained_) {
            auto* blocks_ptr = field(b, cpu, offsetof(CpuState, chain_blocks));
            b.CreateStore(b.CreateAdd(b.CreateLoad(i32, blocks_ptr), b.getInt32(1)), blocks_ptr);
        }
        return true;
    };
    auto slot_of = [&](Value* pc) {
        auto* index = b.CreateAnd(b.CreateLShr(pc, 1), b.getInt32((1u << kChainBits) - 1));
        return b.CreateGEP(i8, table, b.CreateMul(b.CreateZExt(index, b.getInt64Ty()),
                                                  b.getInt64(sizeof(ChainSlot))), "chain_slot");
    };
    // The slot's block runs next if the slot holds pc.
    auto call_slot = [&](Value* slot, Value* pc, Value* total, const Tail& tail, BasicBlock* out,
                         bool did_jump_clear) {
        auto* slot_pc = b.CreateLoad(i32, b.CreateGEP(i8, slot, b.getInt64(offsetof(ChainSlot, pc))), "slot_pc");
        auto* hit = BasicBlock::Create(ctx, "chain_hit", fn);
        b.CreateCondBr(b.CreateICmpEQ(slot_pc, pc), hit, out, weights_usually(ctx));
        b.SetInsertPoint(hit);
        if (!chain(total, tail, pc, did_jump_clear)) return;
        auto* next_fn = b.CreateLoad(ptr, b.CreateGEP(i8, slot, b.getInt64(offsetof(ChainSlot, fn))), "chain_fn");
        commit();
        auto* call = b.CreateCall(fn->getFunctionType(), next_fn, {real, mem});
        call->setCallingConv(CallingConv::GHC);
        call->setTailCallKind(CallInst::TCK_MustTail);
        b.CreateRetVoid();
    };

    // The returns whose next PC is known only at run time, with what they
    // need: a PHI per value, the tail's fields loaded where a return did not
    // skip their stores.
    struct Shared { ReturnInst* ret; Value* next; Value* packets; Tail tail; };
    std::vector<Shared> shared;
    for (auto* ret : returns) {
        Value* next = stored_before(ret, cpu, offsetof(CpuState, pc));
        Value* packets = stored_before(ret, cpu, offsetof(CpuState, packets));
        const Tail tail = sink_tail(ret, cpu);
        b.SetInsertPoint(ret);
        if (!next) next = b.CreateLoad(i32, field(b, cpu, offsetof(CpuState, pc)), "next_pc");
        if (!packets) packets = b.CreateLoad(i32, field(b, cpu, offsetof(CpuState, packets)), "packets");
        auto* known = dyn_cast<ConstantInt>(next);
        if (!known) {
            shared.push_back({ret, next, packets, tail});
            continue;
        }
        auto* go = BasicBlock::Create(ctx, "chain_go", fn);
        BasicBlock* out = leave_for(tail);
        b.SetInsertPoint(ret);
        auto* total = admit(packets, go, out);
        const bool past_guard = steady && steady->count(ret);
        ret->eraseFromParent();
        b.SetInsertPoint(go);
        const bool clear = tail.value[kTailDidJump];
        if (self_loop && known->getZExtValue() == entry_pc) {
            if (chain(total, tail, known, clear)) b.CreateBr(past_guard ? first_entry : self_loop);
        } else {
            call_slot(slot_of(known), known, total, tail, out, clear);
        }
    }
    if (shared.empty()) return;

    auto* exit = BasicBlock::Create(ctx, "chain_exit", fn);
    IRBuilder<> at(exit);
    auto* next = at.CreatePHI(i32, shared.size(), "next_pc");
    auto* packets = at.CreatePHI(i32, shared.size(), "packets");
    Tail tail;
    PHINode* phis[kTailFields] = {};
    for (int f = 0; f < kTailFields; ++f)
        for (auto& from : shared)
            if (from.tail.value[f]) {
                phis[f] = at.CreatePHI(f == kTailDidJump ? i8 : i32, shared.size(), "tail");
                tail.value[f] = phis[f];
                break;
            }
    for (auto& from : shared) {
        b.SetInsertPoint(from.ret);
        next->addIncoming(from.next, from.ret->getParent());
        packets->addIncoming(from.packets, from.ret->getParent());
        for (int f = 0; f < kTailFields; ++f) {
            if (!phis[f]) continue;
            Value* value = from.tail.value[f];
            if (!value) value = b.CreateLoad(phis[f]->getType(), field(b, cpu, kTailOffset[f]));
            phis[f]->addIncoming(value, from.ret->getParent());
        }
        b.CreateBr(exit);
        from.ret->eraseFromParent();
    }

    bool clear = true;
    for (auto& from : shared) clear = clear && from.tail.value[kTailDidJump];
    b.SetInsertPoint(exit);
    auto* go = BasicBlock::Create(ctx, "chain_lookup", fn);
    BasicBlock* out = leave_for(tail);
    b.SetInsertPoint(exit);
    auto* total = admit(packets, go, out);
    b.SetInsertPoint(go);
    if (self_loop) {
        // A hardware loop's back edge to the block's start, mostly.
        auto* self = BasicBlock::Create(ctx, "chain_self", fn);
        auto* other = BasicBlock::Create(ctx, "chain_other", fn);
        b.CreateCondBr(b.CreateICmpEQ(next, b.getInt32(entry_pc)), self, other, weights_loop(ctx));
        b.SetInsertPoint(self);
        if (chain(total, tail, next, clear)) b.CreateBr(self_loop);
        b.SetInsertPoint(other);
    }
    call_slot(slot_of(next), next, total, tail, out, clear);
}
