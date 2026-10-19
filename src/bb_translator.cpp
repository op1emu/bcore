#include "bb_translator.h"

#include <cstddef>
#include <cstdio>
#include <vector>

#include "chain_table.h"
#include "cpu_state.h"
#include "mem.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Analysis/ValueTracking.h>

#include "decoder.h"
#include "lift_visitor.h"
#include "packet_state.h"

using namespace llvm;

BBTranslator::BBTranslator(Memory* mem,
                           bool unlimited, bool fastmem, uint64_t fast_base)
    : mem_(mem), unlimited_(unlimited), fastmem_(fastmem), fast_base_(fast_base) {}

BBTranslateResult BBTranslator::translate(uint32_t pc, const CpuState* hint, bool careful) {
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

    LiftVisitor visitor(builder, cpu_ptr, mem_ptr, mod.get(),
                        mem_, unlimited_,
                        fastmem_, fast_base_, mem_->rawmem_limit());
    visitor.set_hint(hint);
    visitor.set_careful(careful);
    // What the entry guard compares the loop bottoms with: the hint's, as the
    // lifter saw them.
    const uint32_t hint_lb[2] = {hint ? hint->lb[0] : 0, hint ? hint->lb[1] : 0};
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

    BasicBlock* lb_guard = visitor.lb_guard_used()
        ? emit_lb_guard(builder, fn, cpu_ptr, first_entry, pc, last_pc, hint_lb) : nullptr;
    // Before the chain exit turns the returns into branches to it.
    if (lazy_state_) sink_packet_state(*fn, cpu_ptr);
    if (host_cycles_ && visitor.accesses_cycles()) emit_host_cycles(builder, cpu_ptr, first_entry);
    // A native loop where the block may well branch to itself: a jump to its
    // start, or the start of a hardware loop now, unless the filter rejects
    // the block. Elsewhere a jump to itself chains through the table, as to
    // any block.
    const bool self_loop = (visitor.jumps_to(pc) || (hint && (hint->lt[0] == pc || hint->lt[1] == pc))) &&
                           (!chain_filter_ || chain_filter_(pc));
    // A native loop that may have moved a loop bottom passes the guard again.
    BasicBlock* loop_top = lb_guard && !visitor.lb_steady() ? lb_guard : first_entry;
    if (chaining_ && unlimited_)
        emit_chain_exit(builder, fn, cpu_ptr, mem_ptr, pc, self_loop ? loop_top : nullptr,
                        lb_guard ? lb_guard_miss_block(lb_guard) : nullptr);

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
// so Core::run compiles it again careful and runs that in its place. Returns
// the guard's block.
BasicBlock* BBTranslator::emit_lb_guard(IRBuilder<>& b, Function* fn, Value* cpu, BasicBlock* first_entry,
                                        uint32_t first_pc, uint32_t last_pc, const uint32_t hint_lb[2]) {
    auto& ctx = fn->getContext();
    auto& entry = fn->getEntryBlock();
    auto* guard = BasicBlock::Create(ctx, "lb_guard", fn, first_entry);
    auto* miss = BasicBlock::Create(ctx, "lb_guard_miss", fn);
    auto* br = cast<BranchInst>(entry.getTerminator());
    br->setSuccessor(0, guard);

    b.SetInsertPoint(guard);
    auto* i32 = b.getInt32Ty();
    Value* ok = b.getTrue();
    for (int i = 0; i < 2; ++i) {
        auto* lb = b.CreateLoad(i32, field(b, cpu, offsetof(CpuState, lb[0]) + 4 * i), "guard_lb");
        auto* hinted = b.CreateICmpEQ(lb, b.getInt32(hint_lb[i]));
        auto* outside = b.CreateICmpUGT(b.CreateSub(lb, b.getInt32(first_pc)), b.getInt32(last_pc - first_pc));
        ok = b.CreateAnd(ok, b.CreateOr(hinted, outside));
    }
    b.CreateCondBr(ok, first_entry, miss, weights_usually(ctx));

    b.SetInsertPoint(miss);
    b.CreateStore(b.getInt8(1), field(b, cpu, offsetof(CpuState, lb_guard_miss)));
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
void BBTranslator::emit_chain_exit(IRBuilder<>& b, Function* fn, Value* cpu, Value* mem,
                                   uint32_t entry_pc, BasicBlock* self_loop, BasicBlock* direct) {
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
    auto* table = at_entry.CreateLoad(ptr, field(at_entry, cpu, offsetof(CpuState, chain_table)), "chain_table");

    auto* leave = BasicBlock::Create(ctx, "chain_leave", fn);
    b.SetInsertPoint(leave);
    b.CreateRetVoid();

    // The budget test every exit makes: on to `go` with the run's packets
    // including this block's, or leave. No exit finds an interrupt pending:
    // generated code checks one after every instruction that may enable it,
    // and whatever else makes one pending sets the budget to 0 (cec_raise).
    auto admit = [&](Value* packets, BasicBlock* go) {
        auto* done = b.CreateLoad(i32, field(b, cpu, offsetof(CpuState, chain_packets)), "chain_packets");
        auto* total = b.CreateAdd(done, packets, "chain_total");
        // Atomic: another thread may set it to 0, and a native loop with no
        // call in it would otherwise read it once.
        auto* budget = b.CreateLoad(i32, field(b, cpu, offsetof(CpuState, chain_budget)), "chain_budget");
        budget->setAtomic(AtomicOrdering::Monotonic);
        budget->setAlignment(Align(4));
        b.CreateCondBr(b.CreateICmpULT(total, budget), go, leave, weights_usually(ctx));
        return total;
    };
    // What the host does before the next block: retire, then clear the
    // packet index and did_jump (Core::run). False in dry mode, where the
    // block returns instead.
    auto chain = [&](Value* total) {
        if (chain_dry_) {
            b.CreateStore(b.getInt8(1), field(b, cpu, offsetof(CpuState, chain_would)));
            b.CreateRetVoid();
            return false;
        }
        b.CreateStore(total, field(b, cpu, offsetof(CpuState, chain_packets)));
        b.CreateStore(b.getInt32(0), field(b, cpu, offsetof(CpuState, packets)));
        b.CreateStore(b.getInt8(0), field(b, cpu, offsetof(CpuState, did_jump)));
        auto* blocks_ptr = field(b, cpu, offsetof(CpuState, chain_blocks));
        b.CreateStore(b.CreateAdd(b.CreateLoad(i32, blocks_ptr), b.getInt32(1)), blocks_ptr);
        return true;
    };
    auto slot_of = [&](Value* pc) {
        auto* index = b.CreateAnd(b.CreateLShr(pc, 1), b.getInt32((1u << kChainBits) - 1));
        return b.CreateGEP(i8, table, b.CreateMul(b.CreateZExt(index, b.getInt64Ty()),
                                                  b.getInt64(sizeof(ChainSlot))), "chain_slot");
    };
    // The slot's block runs next if the slot holds pc.
    auto call_slot = [&](Value* slot, Value* pc, Value* total) {
        auto* slot_pc = b.CreateLoad(i32, b.CreateGEP(i8, slot, b.getInt64(offsetof(ChainSlot, pc))), "slot_pc");
        auto* hit = BasicBlock::Create(ctx, "chain_hit", fn);
        b.CreateCondBr(b.CreateICmpEQ(slot_pc, pc), hit, leave, weights_usually(ctx));
        b.SetInsertPoint(hit);
        if (!chain(total)) return;
        auto* next_fn = b.CreateLoad(ptr, b.CreateGEP(i8, slot, b.getInt64(offsetof(ChainSlot, fn))), "chain_fn");
        auto* call = b.CreateCall(fn->getFunctionType(), next_fn, {cpu, mem});
        call->setCallingConv(CallingConv::GHC);
        call->setTailCallKind(CallInst::TCK_MustTail);
        b.CreateRetVoid();
    };

    BasicBlock* shared = nullptr;
    PHINode* shared_pc = nullptr;
    PHINode* shared_packets = nullptr;
    for (auto* ret : returns) {
        BasicBlock* from = ret->getParent();
        Value* next = stored_before(ret, cpu, offsetof(CpuState, pc));
        Value* packets = stored_before(ret, cpu, offsetof(CpuState, packets));
        b.SetInsertPoint(ret);
        if (!next) next = b.CreateLoad(i32, field(b, cpu, offsetof(CpuState, pc)), "next_pc");
        if (!packets) packets = b.CreateLoad(i32, field(b, cpu, offsetof(CpuState, packets)), "packets");
        auto* known = dyn_cast<ConstantInt>(next);
        if (!known) {
            if (!shared) {
                shared = BasicBlock::Create(ctx, "chain_exit", fn);
                IRBuilder<> at(shared);
                shared_pc = at.CreatePHI(i32, returns.size(), "next_pc");
                shared_packets = at.CreatePHI(i32, returns.size(), "packets");
            }
            shared_pc->addIncoming(next, from);
            shared_packets->addIncoming(packets, from);
            b.CreateBr(shared);
            ret->eraseFromParent();
            continue;
        }
        auto* go = BasicBlock::Create(ctx, "chain_go", fn);
        auto* total = admit(packets, go);
        ret->eraseFromParent();
        b.SetInsertPoint(go);
        if (self_loop && known->getZExtValue() == entry_pc) {
            if (chain(total)) b.CreateBr(self_loop);
        } else {
            call_slot(slot_of(known), known, total);
        }
    }
    if (!shared) return;

    b.SetInsertPoint(shared);
    auto* go = BasicBlock::Create(ctx, "chain_lookup", fn);
    auto* total = admit(shared_packets, go);
    b.SetInsertPoint(go);
    if (self_loop) {
        // A hardware loop's back edge to the block's start, mostly.
        auto* self = BasicBlock::Create(ctx, "chain_self", fn);
        auto* other = BasicBlock::Create(ctx, "chain_other", fn);
        b.CreateCondBr(b.CreateICmpEQ(shared_pc, b.getInt32(entry_pc)), self, other, weights_loop(ctx));
        b.SetInsertPoint(self);
        if (chain(total)) b.CreateBr(self_loop);
        b.SetInsertPoint(other);
    }
    call_slot(slot_of(shared_pc), shared_pc, total);
}
