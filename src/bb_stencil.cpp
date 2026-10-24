// The stencils of the copy-and-patch tier 0 (cp_translator.h): a packet,
// a block's start and its exit as functions with holes, which
// tools/stencilgen.cpp compiles and takes the code and relocations of.

#include "bb_translator.h"

#include <cstddef>
#include <vector>

#include "cpu_state.h"
#include "mem.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>

#include "decoder.h"
#include "lift_visitor.h"
#include "packet_state.h"

using namespace llvm;

Function* BBTranslator::declare_block(Module& mod, const char* name) {
    auto* i8ptr = Type::getInt8PtrTy(mod.getContext());
    auto* ft = FunctionType::get(Type::getVoidTy(mod.getContext()), {i8ptr, i8ptr}, false);
    auto* fn = Function::Create(ft, Function::ExternalLinkage, name, mod);
    fn->setCallingConv(CallingConv::GHC);
    return fn;
}

namespace {

// A stencil's code: no unwind tables (the translator places no frame
// descriptions; the helpers that reach the host catch, cp_translator.h)
// and no jump tables (data the code would need beside it).
Function* stencil_function(Module& mod, const char* name) {
    auto* fn = BBTranslator::declare_block(mod, name);
    fn->addFnAttr(Attribute::NoUnwind);
    fn->addFnAttr("no-jump-tables", "true");
    return fn;
}

// A musttail call of next in place of the terminator of `block`.
void tail_call(BasicBlock* block, Function* next) {
    auto* fn = block->getParent();
    block->getTerminator()->eraseFromParent();
    IRBuilder<> b(block);
    auto* call = b.CreateCall(next, {fn->getArg(0), fn->getArg(1)});
    call->setCallingConv(CallingConv::GHC);
    call->setTailCallKind(CallInst::TCK_MustTail);
    b.CreateRetVoid();
}

Value* field_ptr(IRBuilder<>& b, Value* cpu, size_t offset) {
    return b.CreateGEP(b.getInt8Ty(), cpu, b.getInt64(offset));
}

}  // namespace

BBTranslator::StencilLift BBTranslator::translate_stencil(Module& mod, const char* name, uint32_t pc,
                                                          const StencilHoles& holes) {
    StencilLift out;
    auto& ctx = mod.getContext();
    declare_externs(mod);
    auto* fn = stencil_function(mod, name);
    Value* cpu = fn->getArg(0);
    auto* entry = BasicBlock::Create(ctx, "entry", fn);
    IRBuilder<> builder(entry);
    LiftVisitor visitor(builder, cpu, fn->getArg(1), &mod, mem_, unlimited_, fastmem_, fast_base_, mem_->rawmem_limit());
    visitor.set_careful(true);
    visitor.set_idle_waits(idle_waits_);
    visitor.set_stencil(holes.pc, pc, holes.fields);

    // As translate_as lifts a packet, between the block's entry (the loads
    // the lifter puts there) and what follows.
    auto* insn_entry = BasicBlock::Create(ctx, "insn_entry", fn);
    auto* insn = BasicBlock::Create(ctx, "insn", fn);
    visitor.set_packet_pc(pc);
    builder.CreateBr(insn_entry);
    builder.SetInsertPoint(insn);
    visitor.carry_reg_cache(entry, insn);
    const int bytes = decodeInstruction(visitor, pc);
    visitor.set_fallthrough_pc(pc + bytes);
    visitor.finalize_pending_exits(pc, bytes);
    BasicBlock* cont = nullptr;
    if (!visitor.is_terminated()) {
        // Unreachable until the packet state is sunk: nothing in the next
        // packet reads what this one stored, as nothing in a block's next
        // packet does before storing it again (its PC is stored at its top,
        // below).
        cont = BasicBlock::Create(ctx, "cont", fn);
        builder.CreateBr(cont);
        builder.SetInsertPoint(cont);
        builder.CreateUnreachable();
    } else if (!builder.GetInsertBlock()->getTerminator()) {
        builder.CreateRetVoid();
    }
    // A packet in a block finds its own PC in pc, which the one before it
    // stored at its end: here the packet stores it itself, as it does its
    // length and index.
    builder.SetInsertPoint(insn_entry);
    builder.CreateStore(holes.pc, field_ptr(builder, cpu, offsetof(CpuState, pc)));
    builder.CreateStore(builder.getInt32(bytes), field_ptr(builder, cpu, offsetof(CpuState, insn_len)));
    builder.CreateStore(holes.packet, field_ptr(builder, cpu, offsetof(CpuState, packets)));
    builder.CreateBr(insn);

    if (lazy_state_) sink_packet_state(*fn, cpu);
    std::vector<BasicBlock*> exits;
    for (auto& block : *fn)
        if (isa<ReturnInst>(block.getTerminator())) exits.push_back(&block);
    for (auto* block : exits) tail_call(block, holes.exit);
    if (cont) tail_call(cont, holes.cont);

    out.fn = fn;
    out.bytes = bytes;
    out.terminated = !cont;
    out.cycles = visitor.accesses_cycles();
    for (unsigned i = 0; i < LiftVisitor::kFieldHoles; ++i) out.field_values[i] = visitor.field_values()[i];
    out.fields_used = visitor.fields_used();
    return out;
}

Function* BBTranslator::emit_stencil_prologue(Module& mod, const char* name, Constant* counter,
                                              Constant* count_offset, Constant* threshold, bool host_cycles,
                                              Function* cont) {
    auto* fn = stencil_function(mod, name);
    Value* cpu = fn->getArg(0);
    auto* first = BasicBlock::Create(mod.getContext(), "first", fn);
    IRBuilder<> b(first);
    b.CreateUnreachable();
    tail_call(first, cont);
    if (host_cycles) emit_host_cycles(b, cpu, first);
    emit_tier_counter(b, cpu, first, counter, count_offset, threshold);
    return fn;
}

Function* BBTranslator::emit_stencil_exit(Module& mod, const char* name, Constant* block_pc, Function* self) {
    auto* fn = stencil_function(mod, name);
    Value* cpu = fn->getArg(0);
    auto* entry = BasicBlock::Create(mod.getContext(), "entry", fn);
    IRBuilder<> b(entry);
    b.CreateRetVoid();
    // The way back to the block's start, as a block's next does through the
    // table: the chain exit's self path (the budget, the retired packets,
    // did_jump), then its first stencil, the prologue, which counts the
    // start and sets CYCLES.
    BasicBlock* again = nullptr;
    if (self && block_pc && chaining_ && unlimited_ && !chain_dry_) {
        again = BasicBlock::Create(mod.getContext(), "self", fn);
        IRBuilder<> at(again);
        at.CreateUnreachable();
        tail_call(again, self);
    }
    if (chaining_ && unlimited_)
        emit_chain_exit(b, fn, cpu, cpu, fn->getArg(1), 0, again, again, nullptr, nullptr, again ? block_pc : nullptr);
    return fn;
}
