// The stencils of the copy-and-patch tier 0 (cp_translator.h): a packet,
// a block's start and its exit as functions with holes, which
// tools/stencilgen.cpp compiles and takes the code and relocations of.

#include "bb_translator.h"
#include "block_abi.h"

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

namespace bcore {

using namespace llvm;

Function* BBTranslator::declare_block(Module& mod, const char* name) { return bcore_abi::make_block(mod, name); }

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
    bcore_abi::tail_call_block(b, next, bcore_abi::block_cpu(fn), bcore_abi::block_mem(fn));
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
    Value* cpu = bcore_abi::block_cpu(fn);
    auto* entry = BasicBlock::Create(ctx, "entry", fn);
    IRBuilder<> builder(entry);
    LiftVisitor visitor(builder, cpu, bcore_abi::block_mem(fn), &mod, mem_, unlimited_, fastmem_, fast_base_, mem_->rawmem_limit());
    visitor.set_careful(true);
    visitor.set_idle_waits(idle_waits_);
    visitor.set_stencil(holes.pc, pc, holes.fields);
    visitor.set_stencil_regs(holes.regs);

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
    out.reg_violation = visitor.reg_hole_violation();
    return out;
}

namespace {

// What a packet stencil ends in, from its lift: the packet state stored at
// its top (sunk), every return an exit, the way on (cont, unreachable as
// the lift left it) the next packet.
void finish_stencil(Function* fn, Value* cpu, BasicBlock* insn_entry, BasicBlock* insn, BasicBlock* cont,
                    const BBTranslator::StencilHoles& holes, Value* len, bool lazy_state) {
    if (insn_entry) {
        IRBuilder<> builder(insn_entry);
        builder.CreateStore(holes.pc, field_ptr(builder, cpu, offsetof(CpuState, pc)));
        builder.CreateStore(len, field_ptr(builder, cpu, offsetof(CpuState, insn_len)));
        builder.CreateStore(holes.packet, field_ptr(builder, cpu, offsetof(CpuState, packets)));
        builder.CreateBr(insn);
    }
    if (lazy_state) sink_packet_state(*fn, cpu);
    std::vector<BasicBlock*> exits;
    for (auto& block : *fn)
        if (isa<ReturnInst>(block.getTerminator())) exits.push_back(&block);
    for (auto* block : exits) tail_call(block, holes.exit);
    if (cont) tail_call(cont, holes.cont);
}

// The way on after a piece's lift: an unreachable block, the cont that
// finish_stencil makes a call of the next piece.
BasicBlock* way_on(IRBuilder<>& builder, Function* fn) {
    auto* cont = BasicBlock::Create(fn->getContext(), "cont", fn);
    builder.CreateBr(cont);
    builder.SetInsertPoint(cont);
    builder.CreateUnreachable();
    return cont;
}

}  // namespace

BBTranslator::StencilLift BBTranslator::translate_stencil_body(Module& mod, const char* name, uint32_t pc,
                                                               const StencilHoles& holes, bool parallel) {
    StencilLift out;
    auto& ctx = mod.getContext();
    declare_externs(mod);
    auto* fn = stencil_function(mod, name);
    auto* entry = BasicBlock::Create(ctx, "entry", fn);
    IRBuilder<> builder(entry);
    LiftVisitor visitor(builder, bcore_abi::block_cpu(fn), bcore_abi::block_mem(fn), &mod, mem_, unlimited_, fastmem_, fast_base_,
                        mem_->rawmem_limit());
    visitor.set_careful(true);
    visitor.set_stencil(holes.pc, pc, holes.fields);
    visitor.set_stencil_regs(holes.regs);
    visitor.set_body_check(!parallel);
    visitor.set_packet_pc(pc);
    visitor.set_pc(pc);
    const uint32_t insn = uint32_t(mem_->read16(pc)) << 16 | mem_->read16(pc + 2);
    if (parallel) visitor.on_parallel_begin();
    const bool legal = decodeInstruction32(visitor, insn);
    if (parallel && legal) visitor.on_parallel_end();
    out.terminated = !legal || visitor.is_terminated();
    BasicBlock* cont = out.terminated ? nullptr : way_on(builder, fn);
    if (!cont && !builder.GetInsertBlock()->getTerminator()) builder.CreateRetVoid();
    finish_stencil(fn, bcore_abi::block_cpu(fn), nullptr, nullptr, cont, holes, nullptr, false);
    out.fn = fn;
    out.bytes = 4;
    out.cycles = visitor.accesses_cycles();
    out.reg_violation = visitor.reg_hole_violation();
    out.reads_after_write = visitor.reads_after_write();
    out.dis_algn = visitor.dis_algn();
    return out;
}

BBTranslator::StencilLift BBTranslator::translate_slot_stencil(Module& mod, const char* name, uint32_t pc,
                                                               const StencilHoles& holes, int slot, bool dis_algn) {
    StencilLift out;
    auto& ctx = mod.getContext();
    declare_externs(mod);
    auto* fn = stencil_function(mod, name);
    Value* cpu = bcore_abi::block_cpu(fn);
    auto* entry = BasicBlock::Create(ctx, "entry", fn);
    IRBuilder<> builder(entry);
    LiftVisitor visitor(builder, cpu, bcore_abi::block_mem(fn), &mod, mem_, unlimited_, fastmem_, fast_base_, mem_->rawmem_limit());
    visitor.set_careful(true);
    visitor.set_idle_waits(idle_waits_);
    visitor.set_stencil(holes.pc, pc, holes.fields);
    auto* insn_entry = BasicBlock::Create(ctx, "insn_entry", fn);
    auto* insn = BasicBlock::Create(ctx, "insn", fn);
    visitor.set_packet_pc(pc);
    builder.CreateBr(insn_entry);
    builder.SetInsertPoint(insn);
    visitor.carry_reg_cache(entry, insn);
    const uint32_t slot_pc = pc + 2 + 2 * uint32_t(slot);
    visitor.begin_slot(slot, dis_algn);
    visitor.set_pc(slot_pc);
    out.legal = decodeInstruction16(visitor, mem_->read16(slot_pc));
    out.writes = visitor.end_slot();
    out.slot_conflict = visitor.slot_conflict();
    out.checks_cec = visitor.checks_cec_pending();
    out.terminated = visitor.is_terminated() || visitor.has_pending_exits();
    out.did_jump_clear = visitor.did_jump_known_clear();
    BasicBlock* cont = out.terminated ? nullptr : way_on(builder, fn);
    if (!cont && !builder.GetInsertBlock()->getTerminator()) builder.CreateRetVoid();
    finish_stencil(fn, cpu, insn_entry, insn, cont, holes, builder.getInt32(8), lazy_state_);
    out.fn = fn;
    out.bytes = 8;
    out.cycles = visitor.accesses_cycles();
    return out;
}

Function* BBTranslator::emit_slot_commit(Module& mod, const char* name, int slot,
                                         const std::vector<LiftVisitor::SlotWrite>& writes, Function* cont) {
    auto* fn = stencil_function(mod, name);
    Value* cpu = bcore_abi::block_cpu(fn);
    auto* entry = BasicBlock::Create(mod.getContext(), "entry", fn);
    IRBuilder<> b(entry);
    auto* i32 = b.getInt32Ty();
    for (size_t j = 0; j < writes.size(); ++j) {
        const size_t scratch =
            offsetof(CpuState, slot_scratch) + sizeof(CpuState::slot_scratch[0]) * size_t(slot - 1) + 4 * j;
        Value* v = b.CreateLoad(i32, field_ptr(b, cpu, scratch), "slot_value");
        Value* at = field_ptr(b, cpu, writes[j].offset);
        if (writes[j].half) {
            Value* old = b.CreateLoad(i32, at, "old");
            v = writes[j].half == 1 ? b.CreateOr(b.CreateAnd(old, b.getInt32(0xFFFF0000)), v)
                                    : b.CreateOr(b.CreateAnd(old, b.getInt32(0x0000FFFF)), b.CreateShl(v, 16));
        }
        b.CreateStore(v, at);
    }
    b.CreateUnreachable();
    tail_call(entry, cont);
    return fn;
}

BBTranslator::StencilLift BBTranslator::translate_parallel_tail(Module& mod, const char* name, uint32_t pc,
                                                                const StencilHoles& holes, bool check_cec) {
    StencilLift out;
    auto& ctx = mod.getContext();
    declare_externs(mod);
    auto* fn = stencil_function(mod, name);
    Value* cpu = bcore_abi::block_cpu(fn);
    auto* entry = BasicBlock::Create(ctx, "entry", fn);
    IRBuilder<> builder(entry);
    LiftVisitor visitor(builder, cpu, bcore_abi::block_mem(fn), &mod, mem_, unlimited_, fastmem_, fast_base_, mem_->rawmem_limit());
    visitor.set_careful(true);
    visitor.set_idle_waits(idle_waits_);
    visitor.set_stencil(holes.pc, pc, holes.fields);
    auto* insn_entry = BasicBlock::Create(ctx, "insn_entry", fn);
    auto* insn = BasicBlock::Create(ctx, "insn", fn);
    visitor.set_packet_pc(pc);
    builder.CreateBr(insn_entry);
    builder.SetInsertPoint(insn);
    visitor.carry_reg_cache(entry, insn);
    visitor.set_pc(pc);
    visitor.set_checks_cec_pending(check_cec);
    visitor.set_fallthrough_pc(pc + 8);
    visitor.finalize_pending_exits(pc, 8);
    BasicBlock* cont = visitor.is_terminated() ? nullptr : way_on(builder, fn);
    if (!cont && !builder.GetInsertBlock()->getTerminator()) builder.CreateRetVoid();
    finish_stencil(fn, cpu, insn_entry, insn, cont, holes, builder.getInt32(8), lazy_state_);
    out.fn = fn;
    out.bytes = 8;
    out.terminated = !cont;
    return out;
}

Function* BBTranslator::emit_stencil_prologue(Module& mod, const char* name, Constant* counter,
                                              Constant* count_offset, Constant* threshold, bool host_cycles,
                                              Function* cont) {
    auto* fn = stencil_function(mod, name);
    Value* cpu = bcore_abi::block_cpu(fn);
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
    Value* cpu = bcore_abi::block_cpu(fn);
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
        emit_chain_exit(b, fn, cpu, cpu, bcore_abi::block_mem(fn), 0, again, again, nullptr, nullptr, again ? block_pc : nullptr);
    return fn;
}

}  // namespace bcore
