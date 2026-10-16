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

#include "decoder.h"
#include "lift_visitor.h"

using namespace llvm;

BBTranslator::BBTranslator(Memory* mem,
                           bool unlimited, bool fastmem, uint64_t fast_base)
    : mem_(mem), unlimited_(unlimited), fastmem_(fastmem), fast_base_(fast_base) {}

BBTranslateResult BBTranslator::translate(uint32_t pc, const CpuState* hint) {
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
    snprintf(fname, sizeof(fname), "bb_0x%08x", pc);

    auto* i8ptr = Type::getInt8PtrTy(*ctx);
    auto* ft = FunctionType::get(Type::getVoidTy(*ctx), {i8ptr, i8ptr}, false);
    auto* fn = Function::Create(ft, Function::ExternalLinkage, fname, mod.get());

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

    uint32_t cur_pc = pc;
    BasicBlock* first_entry = nullptr;  // the first packet's entry
    int max_insns = static_cast<int>(max_packets_); // safety limit per BB
    uint32_t packets = 0;

    while (max_insns-- > 0 && !visitor.is_terminated()) {
        uint32_t insn_pc = cur_pc;
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

    if (host_cycles_ && visitor.accesses_cycles()) emit_host_cycles(builder, cpu_ptr, first_entry);
    // A native loop where the block may well branch to itself: a jump to its
    // start, or the start of a hardware loop now. Elsewhere a jump to itself
    // chains through the table, as to any block.
    const bool self_loop = visitor.jumps_to(pc) || (hint && (hint->lt[0] == pc || hint->lt[1] == pc));
    if (chaining_ && unlimited_) emit_chain_exit(builder, fn, cpu_ptr, mem_ptr, pc, self_loop ? first_entry : nullptr);

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

// Every return goes through one exit that may run the next block itself.
// At each return cpu->pc is where the host would continue, so running that
// block here is a host round trip without the host: allowed while the run's
// budget lasts (the CEC, a halt, the host's memory callbacks and other
// threads set it to 0 when the host has work), no interrupt may be pending
// (the host would call cec_check_pending), and the block is compiled and in
// the chain table, or is this one and self_loop is its first packet (a
// native loop).
void BBTranslator::emit_chain_exit(IRBuilder<>& b, Function* fn, Value* cpu, Value* mem,
                                   uint32_t entry_pc, BasicBlock* self_loop) {
    std::vector<ReturnInst*> returns;
    for (auto& block : *fn)
        if (auto* ret = dyn_cast_or_null<ReturnInst>(block.getTerminator())) returns.push_back(ret);
    if (returns.empty()) return;

    auto& ctx = fn->getContext();
    auto* i8 = b.getInt8Ty();
    auto* i32 = b.getInt32Ty();
    auto* ptr = b.getInt8PtrTy();
    auto* exit = BasicBlock::Create(ctx, "chain_exit", fn);
    auto* pending = BasicBlock::Create(ctx, "chain_pending", fn);
    auto* target = BasicBlock::Create(ctx, "chain_target", fn);
    auto* lookup = BasicBlock::Create(ctx, "chain_lookup", fn);
    auto* leave = BasicBlock::Create(ctx, "chain_leave", fn);
    for (auto* ret : returns) {
        BranchInst::Create(exit, ret);
        ret->eraseFromParent();
    }

    b.SetInsertPoint(exit);
    auto* packets = b.CreateLoad(i32, field(b, cpu, offsetof(CpuState, packets)), "packets");
    auto* done = b.CreateLoad(i32, field(b, cpu, offsetof(CpuState, chain_packets)), "chain_packets");
    auto* total = b.CreateAdd(done, packets, "chain_total");
    // Atomic: another thread may set it to 0, and a native loop with no call
    // in it would otherwise read it once.
    auto* budget = b.CreateLoad(i32, field(b, cpu, offsetof(CpuState, chain_budget)), "chain_budget");
    budget->setAtomic(AtomicOrdering::Monotonic);
    budget->setAlignment(Align(4));
    b.CreateCondBr(b.CreateICmpULT(total, budget), pending, leave);

    b.SetInsertPoint(pending);
    auto* flag = b.CreateLoad(ptr, field(b, cpu, offsetof(CpuState, cec_pending)), "cec_pending_flag");
    auto* is_pending = b.CreateICmpNE(b.CreateLoad(i8, flag, "cec_pending"), b.getInt8(0));
    b.CreateCondBr(is_pending, leave, target);

    b.SetInsertPoint(target);
    auto* next = b.CreateLoad(i32, field(b, cpu, offsetof(CpuState, pc)), "next_pc");
    auto* self = BasicBlock::Create(ctx, "chain_self", fn);
    if (self_loop) b.CreateCondBr(b.CreateICmpEQ(next, b.getInt32(entry_pc)), self, lookup);
    else b.CreateBr(lookup);

    b.SetInsertPoint(lookup);
    auto* table = b.CreateLoad(ptr, field(b, cpu, offsetof(CpuState, chain_table)), "chain_table");
    auto* index = b.CreateAnd(b.CreateLShr(next, 1), b.getInt32((1u << kChainBits) - 1));
    auto* slot = b.CreateGEP(i8, table, b.CreateMul(b.CreateZExt(index, b.getInt64Ty()),
                                                    b.getInt64(sizeof(ChainSlot))), "chain_slot");
    auto* slot_pc = b.CreateLoad(i32, b.CreateGEP(i8, slot, b.getInt64(offsetof(ChainSlot, pc))), "slot_pc");
    auto* hit = BasicBlock::Create(ctx, "chain_hit", fn);
    b.CreateCondBr(b.CreateICmpEQ(slot_pc, next), hit, leave);

    // What the host does before the next block: retire, then clear the
    // packet index and did_jump (Core::run).
    auto chain = [&](BasicBlock* at) {
        b.SetInsertPoint(at);
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
    if (self_loop && chain(self)) b.CreateBr(self_loop);
    if (!self_loop) self->eraseFromParent();
    if (chain(hit)) {
        auto* next_fn = b.CreateLoad(ptr, b.CreateGEP(i8, slot, b.getInt64(offsetof(ChainSlot, fn))), "chain_fn");
        auto* call = b.CreateCall(fn->getFunctionType(), next_fn, {cpu, mem});
        call->setTailCallKind(CallInst::TCK_MustTail);
        b.CreateRetVoid();
    }

    b.SetInsertPoint(leave);
    b.CreateRetVoid();
}
